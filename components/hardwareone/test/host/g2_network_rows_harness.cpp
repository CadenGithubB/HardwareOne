// test_wifi_saved_ui.py also compiles the production G2 Network page's local
// state (row bindings), its WiFi / ESP-NOW / HTTP(S) / Bluetooth / G2 / R1
// menus and their tap handlers beside these host boundaries. A case draws a
// menu, changes the live state its labels came from (as web, CLI, an
// automation or a link event would), taps a row and checks that the command
// matches the label that was on the lens. Command callbacks, leaf pages and
// tasks are recording boundaries. `harness --list`; `harness <case>`.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef ENABLE_WIFI
#define ENABLE_WIFI 1
#endif
#ifndef ENABLE_ESPNOW
#define ENABLE_ESPNOW 1
#endif
#ifndef ENABLE_HTTP_SERVER
#define ENABLE_HTTP_SERVER 1
#endif
#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
#define BROADCAST_PRINTF(...) quietLog(__VA_ARGS__)
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define APP_CORE 1

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)
#define EXPECT(condition, message) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: FAILED: %s\n  (%s)\n", __FILE__, __LINE__, \
               message, #condition); \
  std::abort(); } } while (false)

// INSERT_TYPES

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
  String substring(size_t from, size_t to) const { return String(substr(from, to - from)); }
};
static String operator+(const String& left, const char* right) {
  return String(static_cast<const std::string&>(left) + right);
}

// ---- Clock and lens ------------------------------------------------------------
static uint32_t now = 1000;
uint32_t millis() { return now; }
void vTaskDelay(uint32_t ticks) { now += ticks; }

static uint32_t menuGen = 1;
static G2HijackPage page = G2_HIJACK_PAGE_NETWORK;
uint32_t g2CurrentMenuGen() { return menuGen; }
void g2BumpMenuGen() { ++menuGen; }
G2HijackPage g2GetHijackPage() { return page; }

static bool admit = true;              // false: the lens refuses the next list
static char shownRows[12][80]{};
static size_t shownCount = 0;
bool g2ShowListPage(const char* const* rows, size_t count) {
  CHECK(count <= 12);
  if (!admit) return false;
  shownCount = count;
  for (size_t i = 0; i < count; ++i)
    std::snprintf(shownRows[i], sizeof(shownRows[i]), "%s", rows[i]);
  return true;
}
bool g2EnqueueRedraw(const G2CmdCookie&, void (*)(), const char*) { return true; }

// ---- Commands, keyboard and tasks ----------------------------------------------
static char lastCommand[64]{};
static unsigned submits = 0;
static G2HijackCmdCallback lastCallback = nullptr;
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie&,
                           G2HijackCmdCallback callback, void*) {
  std::snprintf(lastCommand, sizeof(lastCommand), "%s", line);
  lastCallback = callback;
  ++submits;
  return true;
}
static unsigned keyboards = 0;
static TextEntryCommitFn keyboardCommit = nullptr;
bool g2BeginTextEntry(const TextEntryConfig& config) {
  ++keyboards;
  keyboardCommit = config.onCommit;
  return true;
}
static unsigned tasksSpawned = 0;
static void taskStackRecord(const char*, int) {}
static int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int,
                                   void*, int) {
  ++tasksSpawned;
  return pdPASS;
}
static void wifiPendingWatchdogTask(void*) {}
static void ringPendingWatchdogTask(void*) {}

// ---- Radio, server, BLE and settings state --------------------------------------
static bool radioOn = true, linkUp = false;
struct FakeIp { String toString() const { return String("192.0.2.10"); } };
struct FakeWiFi {
  bool isConnected() const { return linkUp; }
  String SSID() const { return String("HomeNet"); }
  FakeIp localIP() const { return FakeIp{}; }
} WiFi;
static bool wifiRadioOn() { return radioOn; }
struct WifiNetworkSnapshot { int count = 0; };

struct FakeEspNow { bool initialized = false; int deviceCount = 0; };
static FakeEspNow espnowState;
static FakeEspNow* gEspNow = &espnowState;
static const char* getEspNowModeString() { return "mesh"; }

static int serverObject = 0;
static void* server = nullptr;
static bool gServerIsHttps = false;
static bool isHttpServerRunning() { return server != nullptr; }

enum { BLE_MODE_SERVER = 0, BLE_MODE_G2_CLIENT = 1 };
enum { BLE_PEER_G2_GLASSES = 0, BLE_PEER_R1_RING = 1 };
static bool serverUp = false, clientUp = true, phoneConnected = false, ringUp = false;
static bool peerAutoReconnect[2] = {false, false};
static bool isBleServerInitialized() { return serverUp; }
static bool isG2ClientInitialized() { return clientUp; }
static bool bleSubsystemActive() { return serverUp || clientUp; }
static const char* bleSubsystemStateString() { return "running"; }
static bool isG2Connected() { return clientUp; }
static bool isBLEConnected() { return phoneConnected; }
static bool blePeerAutoReconnectEnabled(int peer) { return peerAutoReconnect[peer]; }
static bool g2RingIsConnected() { return ringUp; }

struct Settings {
  bool wifiEnabled = true;
  bool wifiAutoStart = false;
  String espnowDeviceName = String("lab");
  bool espnowEnabled = true;
  bool bleAutoStart = false;
  int bleMode = BLE_MODE_G2_CLIENT;
  bool httpsEnabled = false;
  bool httpAutoStart = false;
} gSettings;

// INSERT_STATE

// ---- Views and callbacks outside the row-bound menus ---------------------------
static const char* lastLeaf = "";
static void leafPage(NetworkSubMode sub, const char* name) {
  setNetSub(sub);
  lastLeaf = name;
  const char* rows[] = {"<- Back", name};
  g2ShowListPage(rows, 2);
}
static void g2ShowNetworkMenu() { leafPage(NET_SUB_MAIN, "Network"); }
static void showWiFiStatusPage() { leafPage(NET_SUB_WIFI_STATUS, "Status"); }
static void showWiFiSavedList() { leafPage(NET_SUB_WIFI_SAVED, "Saved"); }
static void showEspNowDevices() { leafPage(NET_SUB_ESPNOW_DEVS, "Devices"); }
static void spawnNetworkScanWorker() { lastLeaf = "Scan"; }

#define CALLBACK(name) \
  static void name(bool, const char*, const G2CmdCookie&, void*) {}
CALLBACK(onWifiMenuRefreshDone)
CALLBACK(onWifiConnectRefreshDone)
CALLBACK(onEspNowMenuRefreshDone)
CALLBACK(onEspNowOpenDone)
CALLBACK(onEspNowNameThenOpenDone)
CALLBACK(onBluetoothMenuRefreshDone)
CALLBACK(onBluetoothG2MenuRefreshDone)
CALLBACK(onBluetoothR1MenuRefreshDone)
CALLBACK(onHttpMenuRefreshDone)
static void showHttpMenu();

// INSERT_FUNCTIONS

// ---- Drivers -------------------------------------------------------------------
static size_t rowOf(const char* text) {
  for (size_t i = 0; i < shownCount; ++i)
    if (std::strstr(shownRows[i], text)) return i;
  std::fprintf(stderr, "row '%s' not on the lens\n", text);
  std::abort();
}
static void clearCommands() {
  lastCommand[0] = '\0';
  submits = 0;
  lastCallback = nullptr;
  keyboards = 0;
  keyboardCommit = nullptr;
}
static bool sent(const char* line) {
  return submits == 1 && std::strcmp(lastCommand, line) == 0;
}
static uint32_t tapRow(size_t row) {
  clearCommands();
  return (uint32_t)row;
}

// ---- ESP-NOW ---------------------------------------------------------------------
// The work item's case: ON layout drawn, ESP-NOW closed from web/CLI/automation,
// "View Devices" (row 4) tapped.
static void espnow_view_devices_after_stop() {
  espnowState = FakeEspNow{true, 2};
  showEspNowMenu();
  const size_t row = rowOf("View Devices");
  CHECK(row == 4);
  espnowState.initialized = false;
  handleEspNowTap(tapRow(row));
  EXPECT(submits == 0,
         "'View Devices' submitted a command (espnowenabled) after ESP-NOW stopped");
  EXPECT(gNetSub == NET_SUB_ESPNOW_DEVS, "'View Devices' did not open the device list");
}

static void espnow_peers_row_after_stop() {
  espnowState = FakeEspNow{true, 2};
  showEspNowMenu();
  const size_t row = rowOf("Peers: 2");
  espnowState.initialized = false;
  handleEspNowTap(tapRow(row));
  EXPECT(keyboards == 0 && submits == 0,
         "'Peers: N' (info row) opened the name keyboard after ESP-NOW stopped");
}

static void espnow_toggle_follows_label() {
  espnowState = FakeEspNow{true, 0};
  showEspNowMenu();
  size_t row = rowOf("ESP-NOW: ON");
  espnowState.initialized = false;
  handleEspNowTap(tapRow(row));
  EXPECT(sent("closeespnow"), "'ESP-NOW: ON' did not send closeespnow after ESP-NOW stopped");

  showEspNowMenu();
  row = rowOf("ESP-NOW: OFF");
  espnowState.initialized = true;
  handleEspNowTap(tapRow(row));
  EXPECT(sent("openespnow") && lastCallback == &onEspNowOpenDone,
         "'ESP-NOW: OFF' did not send openespnow after ESP-NOW started");
}

static void espnow_auto_start_follows_label() {
  gSettings.espnowEnabled = true;
  showEspNowMenu();                          // OFF layout: Auto Start is row 4
  size_t row = rowOf("Auto Start: ON");
  espnowState.initialized = true;            // started meanwhile: ON layout row 4 = View Devices
  handleEspNowTap(tapRow(row));
  EXPECT(sent("espnowenabled 0"),
         "'Auto Start: ON' (OFF layout) did not turn Auto Start off after ESP-NOW started");

  showEspNowMenu();
  row = rowOf("Auto Start: ON");
  gSettings.espnowEnabled = false;           // turned off from the web meanwhile
  handleEspNowTap(tapRow(row));
  EXPECT(sent("espnowenabled 0"),
         "'Auto Start: ON' sent espnowenabled 1 after the setting changed");
}

static void espnow_rows_unchanged_state() {
  espnowState = FakeEspNow{true, 3};
  showEspNowMenu();
  const char* on[] = {"<- WiFi", "ESP-NOW: ON", "Mode: mesh", "Peers: 3",
                      "View Devices", "Name: lab", "Auto Start: ON"};
  CHECK(shownCount == 7);
  for (size_t i = 0; i < 7; ++i) CHECK(std::strcmp(shownRows[i], on[i]) == 0);
  handleEspNowTap(tapRow(1));
  CHECK(sent("closeespnow") && lastCallback == &onEspNowMenuRefreshDone);
  handleEspNowTap(tapRow(2));
  handleEspNowTap(tapRow(3));
  CHECK(submits == 0 && keyboards == 0);
  handleEspNowTap(tapRow(5));
  CHECK(keyboards == 1 && keyboardCommit == &espnowNameCommit && submits == 0);
  handleEspNowTap(tapRow(6));
  CHECK(sent("espnowenabled 0"));
  handleEspNowTap(tapRow(4));
  CHECK(gNetSub == NET_SUB_ESPNOW_DEVS && submits == 0);

  espnowState.initialized = false;
  gSettings.espnowDeviceName = String("");
  showEspNowMenu();
  const char* off[] = {"<- WiFi", "ESP-NOW: OFF", "View Devices", "Name: (unset)",
                       "Auto Start: ON"};
  CHECK(shownCount == 5);
  for (size_t i = 0; i < 5; ++i) CHECK(std::strcmp(shownRows[i], off[i]) == 0);
  handleEspNowTap(tapRow(1));                // unnamed: keyboard first, then open
  CHECK(keyboards == 1 && keyboardCommit == &espnowNameThenOpenCommit && submits == 0);
  handleEspNowTap(tapRow(3));
  CHECK(keyboards == 1 && keyboardCommit == &espnowNameCommit);
  gSettings.espnowDeviceName = String("lab");
  showEspNowMenu();
  handleEspNowTap(tapRow(1));
  CHECK(sent("openespnow") && lastCallback == &onEspNowOpenDone);
  handleEspNowTap(tapRow(4));
  CHECK(sent("espnowenabled 0"));
  handleEspNowTap(tapRow(0));
  CHECK(gNetSub == NET_SUB_WIFI && submits == 0);
  showEspNowMenu();
  handleEspNowTap(tapRow(2));
  CHECK(gNetSub == NET_SUB_ESPNOW_DEVS);
}

// A refused render after a submenu change leaves the previous list on the lens
// while gNetSub already names the new submenu; its rows must not run.
static void refused_render_after_navigation() {
  espnowState = FakeEspNow{true, 2};
  showEspNowMenu();
  handleEspNowTap(tapRow(rowOf("View Devices")));
  CHECK(gNetSub == NET_SUB_ESPNOW_DEVS);
  admit = false;
  showEspNowMenu();                          // "<- ESP-NOW" refused: devices still shown
  admit = true;
  CHECK(std::strcmp(shownRows[1], "Devices") == 0);
  handleEspNowTap(tapRow(1));                // a device row on the lens
  EXPECT(submits == 0 && keyboards == 0,
         "a tap on the still-visible device list ran the ESP-NOW menu's row 1 (closeespnow)");
}

// ---- HTTP(S) ---------------------------------------------------------------------
static void http_server_row_follows_label() {
  linkUp = true;
  server = &serverObject;
  showHttpMenu();
  size_t row = rowOf("Stop HTTP");
  server = nullptr;                          // stopped from the web / CLI meanwhile
  handleHttpTap(tapRow(row));
  EXPECT(sent("closehttp"), "'Stop HTTP' did not send closehttp after the server stopped");

  showHttpMenu();
  row = rowOf("Start HTTP");
  server = &serverObject;
  handleHttpTap(tapRow(row));
  EXPECT(sent("openhttp"), "'Start HTTP' did not send openhttp after the server started");
}

static void http_toggles_follow_label() {
  linkUp = true;
  showHttpMenu();
  size_t row = rowOf("HTTPS: OFF");
  gSettings.httpsEnabled = true;
  handleHttpTap(tapRow(row));
  EXPECT(sent("httpsEnabled 1"), "'HTTPS: OFF' sent httpsEnabled 0 after the setting changed");

  gSettings.httpAutoStart = true;
  showHttpMenu();
  row = rowOf("Auto Start: ON");
  gSettings.httpAutoStart = false;
  handleHttpTap(tapRow(row));
  EXPECT(sent("httpAutoStart 0"), "'Auto Start: ON' sent httpAutoStart 1 after the setting changed");
}

static void http_rows_unchanged_state() {
  linkUp = true;
  server = &serverObject;
  showHttpMenu();
  const char* rows[] = {"<- WiFi", "Stop HTTP", "HTTPS: OFF (reboot req.)",
                        "Auto Start: OFF", "URL: http://192.0.2.10"};
  CHECK(shownCount == 5);
  for (size_t i = 0; i < 5; ++i) CHECK(std::strcmp(shownRows[i], rows[i]) == 0);
  handleHttpTap(tapRow(4));
  CHECK(submits == 0);
  handleHttpTap(tapRow(2));
  CHECK(sent("httpsEnabled 1"));
  handleHttpTap(tapRow(3));
  CHECK(sent("httpAutoStart 1"));
  handleHttpTap(tapRow(1));
  CHECK(sent("closehttp"));
  server = nullptr;
  linkUp = false;
  showHttpMenu();
  CHECK(shownCount == 4 && std::strcmp(shownRows[1], "Start HTTP (WiFi req.)") == 0);
  handleHttpTap(tapRow(1));
  CHECK(sent("openhttp"));
  handleHttpTap(tapRow(9));
  CHECK(submits == 0);
  handleHttpTap(tapRow(0));
  CHECK(gNetSub == NET_SUB_WIFI && submits == 0);
}

// ---- WiFi --------------------------------------------------------------------------
static void wifi_pending_row_after_link_up() {
  armWifiPendingOverlay();
  showWiFiMenu();
  const size_t row = rowOf("WiFi: Pending...");
  linkUp = true;                             // the link comes up just before the tap
  handleWiFiTap(tapRow(row));
  EXPECT(std::strcmp(lastCommand, "closewifi") != 0,
         "a tap on 'WiFi: Pending...' sent closewifi once the link came up");
  EXPECT(sent("openwifi"), "a tap on 'WiFi: Pending...' did not re-run openwifi");
}

static void wifi_connected_row_after_drop() {
  linkUp = true;
  showWiFiMenu();
  const size_t row = rowOf("WiFi: HomeNet");
  linkUp = false;
  handleWiFiTap(tapRow(row));
  EXPECT(sent("closewifi"), "'WiFi: <SSID>' started a connect after the link dropped");
}

static void wifi_radio_and_auto_start_follow_label() {
  showWiFiMenu();
  size_t row = rowOf("Radio: ON");
  radioOn = false;                           // radiopower off from the web meanwhile
  handleWiFiTap(tapRow(row));
  EXPECT(sent("radiopower off"), "'Radio: ON' sent radiopower on after the radio went off");

  showWiFiMenu();
  row = rowOf("Auto Start: OFF");
  gSettings.wifiAutoStart = true;
  handleWiFiTap(tapRow(row));
  EXPECT(sent("wifiautostart 1"), "'Auto Start: OFF' sent wifiautostart 0 after the setting changed");
}

static void wifi_rows_unchanged_state() {
  linkUp = true;
  showWiFiMenu();
  const char* rows[] = {"<- Connect", "Radio: ON", "WiFi: HomeNet", "Disconnect", "Status",
                        "Scan Networks", "List Saved", "Auto Start: OFF", "HTTP(S) >>",
                        "ESP-NOW >>"};
  CHECK(shownCount == 10);
  for (size_t i = 0; i < 10; ++i) CHECK(std::strcmp(shownRows[i], rows[i]) == 0);
  handleWiFiTap(tapRow(1));
  CHECK(sent("radiopower off"));
  handleWiFiTap(tapRow(2));
  CHECK(sent("closewifi"));
  handleWiFiTap(tapRow(3));
  CHECK(sent("wifidisconnect"));
  handleWiFiTap(tapRow(5));
  CHECK(std::strcmp(lastLeaf, "Scan") == 0 && submits == 0);
  handleWiFiTap(tapRow(7));
  CHECK(sent("wifiautostart 1"));
  handleWiFiTap(tapRow(8));
  CHECK(gNetSub == NET_SUB_HTTP);
  showWiFiMenu();
  handleWiFiTap(tapRow(9));
  CHECK(gNetSub == NET_SUB_ESPNOW);
  showWiFiMenu();
  handleWiFiTap(tapRow(6));
  CHECK(gNetSub == NET_SUB_WIFI_SAVED);
  showWiFiMenu();
  handleWiFiTap(tapRow(4));
  CHECK(gNetSub == NET_SUB_WIFI_STATUS);

  linkUp = false;
  gSettings.wifiEnabled = false;
  showWiFiMenu();
  const size_t disabled = rowOf("WiFi: Disabled in Settings");
  handleWiFiTap(tapRow(disabled));
  CHECK(submits == 0 && gNetSub == NET_SUB_WIFI);
  gSettings.wifiEnabled = true;
  showWiFiMenu();
  handleWiFiTap(tapRow(rowOf("WiFi: Disconnected")));
  CHECK(sent("openwifi") && lastCallback == &onWifiConnectRefreshDone);
  CHECK(rowOf("WiFi: Pending...") == 2);
  showWiFiMenu();
  handleWiFiTap(tapRow(0));
  CHECK(gNetSub == NET_SUB_MAIN && submits == 0);
}

// ---- Bluetooth, G2, R1 -------------------------------------------------------------
static void r1_connect_row_after_ring_up() {
  showBluetoothR1Menu();
  const size_t row = rowOf("Connect Ring");
  ringUp = true;                             // auto-reconnect got there first
  handleBluetoothR1Tap(tapRow(row));
  EXPECT(std::strcmp(lastCommand, "ringconnect reconnect") != 0,
         "'Connect Ring' sent 'ringconnect reconnect' (drops the ring) after it connected");
  EXPECT(sent("ringconnect"), "'Connect Ring' did not send ringconnect");
}

static void ble_peer_toggles_follow_label() {
  showBluetoothG2Menu();
  size_t row = rowOf("G2 AutoReconnect: OFF");
  peerAutoReconnect[BLE_PEER_G2_GLASSES] = true;
  handleBluetoothG2Tap(tapRow(row));
  EXPECT(sent("bleautoreconnect g2-glasses on"),
         "'G2 AutoReconnect: OFF' sent off after the setting changed");

  showBluetoothR1Menu();
  row = rowOf("R1 AutoReconnect: OFF");
  peerAutoReconnect[BLE_PEER_R1_RING] = true;
  handleBluetoothR1Tap(tapRow(row));
  EXPECT(sent("bleautoreconnect r1-ring on"),
         "'R1 AutoReconnect: OFF' sent off after the setting changed");
}

// Server role only: the layout shows Conn / Toggle Adv / Disconnect while it runs.
static void bluetooth_server_rows_after_stop() {
  clientUp = false;
  serverUp = true;
  phoneConnected = true;
  gSettings.bleMode = BLE_MODE_SERVER;
  showBluetoothMenu();
  CHECK(shownCount == 7);
  const size_t conn = rowOf("Conn: yes"), state = rowOf("BLE: ON"),
               autoStart = rowOf("Auto Start: OFF");
  serverUp = false;                          // closeble from the web meanwhile
  handleBluetoothTap(tapRow(conn));
  EXPECT(submits == 0, "'Conn: yes' (info row) toggled bleautostart after the server stopped");
  handleBluetoothTap(tapRow(state));
  EXPECT(sent("closeble"), "'BLE: ON' sent openble after the server stopped");
  handleBluetoothTap(tapRow(autoStart));
  EXPECT(sent("bleautostart on"), "'Auto Start: OFF' (row 6) did nothing after the server stopped");
}

static void bluetooth_mode_follows_label() {
  clientUp = false;
  gSettings.bleMode = BLE_MODE_SERVER;
  showBluetoothMenu();
  const size_t row = rowOf("Mode: server");
  gSettings.bleMode = BLE_MODE_G2_CLIENT;    // blemode client from the web meanwhile
  handleBluetoothTap(tapRow(row));
  EXPECT(sent("blemode client"), "'Mode: server' sent 'blemode server' after the mode changed");
}

static void bluetooth_rows_unchanged_state() {
  // Client role running.
  showBluetoothMenu();
  const char* client[] = {"<- Connect", "BLE: ON (running)", "Mode: client (G2)", "G2 >>",
                          "R1 >>", "Auto Start: OFF"};
  CHECK(shownCount == 6);
  for (size_t i = 0; i < 6; ++i) CHECK(std::strcmp(shownRows[i], client[i]) == 0);
  handleBluetoothTap(tapRow(1));
  CHECK(sent("g2deinit"));
  handleBluetoothTap(tapRow(2));
  CHECK(sent("blemode server"));
  handleBluetoothTap(tapRow(5));
  CHECK(sent("bleautostart on"));
  handleBluetoothTap(tapRow(3));
  CHECK(gNetSub == NET_SUB_BLUETOOTH_G2 && submits == 0);
  showBluetoothMenu();
  handleBluetoothTap(tapRow(4));
  CHECK(gNetSub == NET_SUB_BLUETOOTH_R1 && submits == 0);

  // Server role running, then stopped; client role stopped.
  clientUp = false;
  serverUp = true;
  gSettings.bleMode = BLE_MODE_SERVER;
  showBluetoothMenu();
  handleBluetoothTap(tapRow(3));
  CHECK(submits == 0);
  handleBluetoothTap(tapRow(4));
  CHECK(sent("bleadv toggle"));
  phoneConnected = true;
  handleBluetoothTap(tapRow(5));
  CHECK(sent("bledisconnect"));
  phoneConnected = false;
  handleBluetoothTap(tapRow(5));             // nobody connected: no-op and redraw
  CHECK(submits == 0 && gNetSub == NET_SUB_BLUETOOTH);
  handleBluetoothTap(tapRow(6));
  CHECK(sent("bleautostart on"));
  serverUp = false;
  showBluetoothMenu();
  CHECK(shownCount == 4 && std::strcmp(shownRows[3], "Auto Start: OFF") == 0);
  handleBluetoothTap(tapRow(1));
  CHECK(sent("openble"));
  handleBluetoothTap(tapRow(3));
  CHECK(sent("bleautostart on"));
  gSettings.bleMode = BLE_MODE_G2_CLIENT;
  showBluetoothMenu();
  CHECK(shownCount == 6);
  handleBluetoothTap(tapRow(1));
  CHECK(sent("openg2"));
  handleBluetoothTap(tapRow(0));
  CHECK(gNetSub == NET_SUB_MAIN && submits == 0);
}

static void g2_r1_rows_unchanged_state() {
  showBluetoothG2Menu();
  CHECK(shownCount == 5 && std::strcmp(shownRows[3], "Reconnect G2") == 0);
  handleBluetoothG2Tap(tapRow(1));
  CHECK(submits == 0);
  handleBluetoothG2Tap(tapRow(3));
  CHECK(sent("openg2 saved"));
  handleBluetoothG2Tap(tapRow(4));
  CHECK(sent("closeg2"));
  handleBluetoothG2Tap(tapRow(2));
  CHECK(sent("bleautoreconnect g2-glasses on"));
  handleBluetoothG2Tap(tapRow(0));
  CHECK(gNetSub == NET_SUB_BLUETOOTH && submits == 0);

  ringUp = true;
  showBluetoothR1Menu();
  CHECK(shownCount == 5 && std::strcmp(shownRows[1], "Ring: connected") == 0);
  handleBluetoothR1Tap(tapRow(3));
  CHECK(sent("ringconnect reconnect") && tasksSpawned == 0);
  handleBluetoothR1Tap(tapRow(4));
  CHECK(sent("ringdisconnect"));
  ringUp = false;
  showBluetoothR1Menu();
  CHECK(shownCount == 4 && std::strcmp(shownRows[3], "Connect Ring") == 0);
  handleBluetoothR1Tap(tapRow(3));
  CHECK(sent("ringconnect") && tasksSpawned == 1);
  CHECK(std::strcmp(shownRows[1], "Ring: connecting...") == 0);
  handleBluetoothR1Tap(tapRow(2));
  CHECK(sent("bleautoreconnect r1-ring on"));
  handleBluetoothR1Tap(tapRow(7));
  CHECK(submits == 0);
}

struct Case { const char* name; void (*run)(); };
static const Case kCases[] = {
  {"espnow_view_devices_after_stop", espnow_view_devices_after_stop},
  {"espnow_peers_row_after_stop", espnow_peers_row_after_stop},
  {"espnow_toggle_follows_label", espnow_toggle_follows_label},
  {"espnow_auto_start_follows_label", espnow_auto_start_follows_label},
  {"refused_render_after_navigation", refused_render_after_navigation},
  {"http_server_row_follows_label", http_server_row_follows_label},
  {"http_toggles_follow_label", http_toggles_follow_label},
  {"wifi_pending_row_after_link_up", wifi_pending_row_after_link_up},
  {"wifi_connected_row_after_drop", wifi_connected_row_after_drop},
  {"wifi_radio_and_auto_start_follow_label", wifi_radio_and_auto_start_follow_label},
  {"r1_connect_row_after_ring_up", r1_connect_row_after_ring_up},
  {"ble_peer_toggles_follow_label", ble_peer_toggles_follow_label},
  {"bluetooth_server_rows_after_stop", bluetooth_server_rows_after_stop},
  {"bluetooth_mode_follows_label", bluetooth_mode_follows_label},
  {"espnow_rows_unchanged_state", espnow_rows_unchanged_state},
  {"http_rows_unchanged_state", http_rows_unchanged_state},
  {"wifi_rows_unchanged_state", wifi_rows_unchanged_state},
  {"bluetooth_rows_unchanged_state", bluetooth_rows_unchanged_state},
  {"g2_r1_rows_unchanged_state", g2_r1_rows_unchanged_state},
};

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
    for (const Case& c : kCases) std::puts(c.name);
    return 0;
  }
  if (argc == 2) {
    for (const Case& c : kCases) {
      if (std::strcmp(argv[1], c.name) != 0) continue;
      c.run();
      std::printf("%s: ok (%u checks)\n", c.name, checks);
      return 0;
    }
  }
  std::fprintf(stderr, "usage: harness --list | harness <case>\n");
  return 2;
}
