// Host boundaries around the lens "Peers" rows, compiled with the production
// code test_g2_espnow_peer_rows.py lifts from G2_Page_Network.cpp
// (g2EspNowPeerCounts, g2BuildNetworkInfo, showEspNowDevices) and
// G2_Page_ESPNow.cpp (the row table, g2BuildEspNowAppInfo, showMainMenu,
// showPeersMenu, showStatsMenu, handlePeersTap). The ESP-NOW registry, the
// session test, the self-address test and the lens list page are doubles.
// Built twice: ENABLE_ESPNOW=1 (the cases below) and ENABLE_ESPNOW=0, where no
// registry, session or self-address double exists, so the lifted code may not
// name one outside its #if.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_MacText.h"  // production macToDisplay (the list's last-resort label)

#ifndef ENABLE_ESPNOW
#define ENABLE_ESPNOW 1
#endif
#define ENABLE_WIFI 0  // the WiFi rows of the lifted text builders are not under test
#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
  std::exit(1); } } while (false)

// ---- Arduino ---------------------------------------------------------------------
class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
};

// ---- The lens list page: the rows of the last page, and how many were shown --------
static std::vector<std::string> shown;
static unsigned pages = 0;
static bool lensRefuses = false;  // the lens does not take the next pages (the old one stays)
static bool g2ShowListPage(const char* const* items, size_t count) {
  if (lensRefuses) return false;
  shown.assign(items, items + count);
  ++pages;
  return true;
}
static unsigned menuGen = 0;
static void g2BumpMenuGen() { ++menuGen; }

// ---- ESP-NOW (System_ESPNow.h), as far as the lifted code reads it ------------------
#if ENABLE_ESPNOW
struct InlineText {  // EspNowInlineText<N>
  std::string text;
  size_t length() const { return text.size(); }
  const char* c_str() const { return text.c_str(); }
};
struct EspNowDevice {
  uint8_t mac[6];
  InlineText name, friendlyName, room;
};
struct RouterMetrics { uint32_t messagesSent = 0, messagesReceived = 0, messagesFailed = 0; };
struct EspNowState {
  bool initialized = false;
  int deviceCount = 0;
  EspNowDevice devices[16] = {};
  // Peers with message history: 9 here and never a row (the lens used to show it).
  int peerHistoryCount = 9;
  RouterMetrics routerMetrics;
  uint8_t channel = 6;
};
static EspNowState espnow;
static EspNowState* gEspNow = &espnow;
static const char* getEspNowModeString() { return "mesh"; }
struct Settings { uint8_t espnowChannel = 0; };
static Settings gSettings;

static uint8_t selfMac[6] = {0x02, 0, 0, 0, 0, 0xEE};
static unsigned selfChecks = 0;
static bool isSelfMac(const uint8_t* mac) {
  ++selfChecks;
  return mac && std::memcmp(mac, selfMac, 6) == 0;
}
// The peers with an ACTIVE session, by the last address byte.
static std::vector<uint8_t> sessionsUp;
static bool espnowSessionUp(const uint8_t* mac, uint32_t* = nullptr) {
  CHECK(std::memcmp(mac, selfMac, 6) != 0);  // never asked about this device
  for (uint8_t id : sessionsUp) if (mac[5] == id) return true;
  return false;
}
static std::vector<uint8_t> roomsPending;  // peers whose announced room is not accepted
static bool espnowGroupRoomAccepted(const uint8_t mac[6], const char*) {
  for (uint8_t id : roomsPending) if (mac[5] == id) return false;
  return true;
}
static int countInboxAll() { return 4; }
#endif

// ---- The pages' own state the lifted functions use ----------------------------------
enum NetworkSubMode : uint8_t { NET_SUB_ESPNOW_DEVS = 6 };
static int netSub = -1;
static void setNetSub(NetworkSubMode sub) { netSub = sub; }
static int gStatsChannelRow = -1;
static unsigned details = 0;
static void showPeerDetail() { ++details; }
static void showMainMenu();
static void showPeersMenu();

// INSERT_PRODUCTION

// ---- Cases -------------------------------------------------------------------------
static void counts(int* up, int* paired) {
  *up = *paired = -7;
  g2EspNowPeerCounts(up, paired);
}

#if ENABLE_ESPNOW
static void setDevices(std::initializer_list<uint8_t> ids) {
  espnow = EspNowState();
  espnow.initialized = true;
  for (uint8_t id : ids) {
    EspNowDevice& d = espnow.devices[espnow.deviceCount++];
    if (id == 0xEE) {
      std::memcpy(d.mac, selfMac, 6);
      d.name.text = "this-device";
    } else {
      const uint8_t mac[6] = {0x02, 0, 0, 0, 0, id};
      std::memcpy(d.mac, mac, 6);
      d.name.text = "peer-" + std::to_string(id);
    }
  }
}
static int indexOf(uint8_t id) {
  for (int i = 0; i < espnow.deviceCount; i++) if (espnow.devices[i].mac[5] == id) return i;
  return -1;
}
static void removeDevice(uint8_t id) {  // the registry closes the gap, as removeEspNowDevice does
  const int at = indexOf(id);
  CHECK(at >= 0);
  for (int i = at; i + 1 < espnow.deviceCount; i++) espnow.devices[i] = espnow.devices[i + 1];
  espnow.deviceCount--;
}
static bool anyRowHas(const char* text) {
  for (const auto& row : shown) if (row.find(text) != std::string::npos) return true;
  return false;
}
static std::string textOf(void (*build)(char*, size_t)) {
  char out[512];
  build(out, sizeof(out));
  return out;
}

// The count: every registry entry except this device is a peer; one is up
// while it holds an ACTIVE session. The status line's rule.
static void countRule() {
  int up, paired;
  espnow = EspNowState();  // not running
  sessionsUp = {1, 2};
  counts(&up, &paired);
  CHECK(up == 0 && paired == 0);
  setDevices({0xEE, 1, 2, 3});
  counts(&up, &paired);
  CHECK(up == 2 && paired == 3);
  setDevices({1, 2, 0xEE, 3});  // this device's entry anywhere in the registry
  sessionsUp = {3};
  counts(&up, &paired);
  CHECK(up == 1 && paired == 3);
  setDevices({0xEE});  // only this device: no peer
  counts(&up, &paired);
  CHECK(up == 0 && paired == 0);
  setDevices({1, 2});  // no own entry yet (name not applied)
  sessionsUp = {1, 2};
  counts(&up, &paired);
  CHECK(up == 2 && paired == 2);
  // A count past the 16 entries the registry holds reads 16, never beyond.
  setDevices({0xEE, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
  espnow.deviceCount = 40;
  sessionsUp = {15};
  counts(&up, &paired);
  CHECK(up == 1 && paired == 15);
  // Either result may be left out.
  g2EspNowPeerCounts(nullptr, &paired);
  g2EspNowPeerCounts(&up, nullptr);
  g2EspNowPeerCounts(nullptr, nullptr);
  CHECK(up == 1 && paired == 15);
  // Stopped: nothing is counted from a registry that is still filled.
  espnow.initialized = false;
  counts(&up, &paired);
  CHECK(up == 0 && paired == 0);
}

// The five rows that print the count show up/paired, and none shows the
// registry size (4 here) or the peers with message history (9).
static void countRows() {
  setDevices({0xEE, 1, 2, 3});
  sessionsUp = {1, 3};
  showMainMenu();
  CHECK(shown.size() == 7 && shown[2] == "Peers (2/3 up) >>" && shown[3] == "Inbox (4) >>");
  showStatsMenu();
  CHECK(anyRowHas("Peers: 2/3 up") && !anyRowHas("Peers: 4") && !anyRowHas("Peers: 9"));
  const std::string app = textOf(g2BuildEspNowAppInfo);
  CHECK(app.find("Peers: 2/3 up\n") != std::string::npos && app.find("Peers: 4") == std::string::npos);
  const std::string net = textOf(g2BuildNetworkInfo);
  CHECK(net.find("ESPNow mesh\nPeers 2/3 up\n") != std::string::npos &&
        net.find("Peers 9") == std::string::npos && net.find("Peers 4") == std::string::npos);
  // The widest counts fit their row buffers whole.
  setDevices({0xEE, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
  sessionsUp = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  showMainMenu();
  CHECK(shown[2] == "Peers (15/15 up) >>");
  showStatsMenu();
  CHECK(anyRowHas("Peers: 15/15 up"));
  // Not running: 0/0, and the text builders print no Peers line at all.
  espnow.initialized = false;
  showMainMenu();
  CHECK(shown[2] == "Peers (0/0 up) >>");
  CHECK(textOf(g2BuildEspNowAppInfo).find("Peers") == std::string::npos);
  CHECK(textOf(g2BuildNetworkInfo).find("ESPNow: off\n") != std::string::npos);
}

// The Peers list has one row per peer and none for this device, so it is as
// long as the count; a row's label is the friendly name, else the name, else
// the address.
static void peersList() {
  setDevices({1, 0xEE, 2, 3});
  espnow.devices[indexOf(2)].friendlyName.text = "Bench Two";
  espnow.devices[indexOf(3)].name.text.clear();
  showPeersMenu();
  CHECK(shown.size() == 4 && shown[0] == "<- Back" && shown[1] == "peer-1" && shown[2] == "Bench Two" &&
        shown[3] == "02:00:00:00:00:03");
  CHECK(!anyRowHas("this-device") && gPeerRowCount == 3);
  int up, paired;
  counts(&up, &paired);
  CHECK(paired == int(shown.size()) - 1);
  setDevices({0xEE, 1, 2});  // this device's entry first, then last
  showPeersMenu();
  CHECK(shown.size() == 3 && shown[1] == "peer-1" && shown[2] == "peer-2" && gPeerRowCount == 2);
  setDevices({1, 2, 0xEE});
  showPeersMenu();
  CHECK(shown.size() == 3 && shown[1] == "peer-1" && shown[2] == "peer-2" && gPeerRowCount == 2);
  // A full registry: 15 peers, 15 rows, no write past the row table.
  setDevices({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0xEE});
  espnow.deviceCount = 40;
  showPeersMenu();
  CHECK(shown.size() == 16 && shown[15] == "peer-15" && gPeerRowCount == 15);
  setDevices({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});  // no own entry
  showPeersMenu();
  CHECK(shown.size() == 17 && shown[16] == "peer-16" && gPeerRowCount == 16);
  // Only this device, or not running: the empty list, and no row left to tap.
  setDevices({0xEE});
  showPeersMenu();
  CHECK(shown.size() == 2 && shown[1] == "(no paired peers)" && gPeerRowCount == 0);
  setDevices({1, 2});
  espnow.initialized = false;
  showPeersMenu();
  CHECK(shown.size() == 2 && shown[1] == "(no paired peers)" && gPeerRowCount == 0);
}

// A tap selects the peer its row named: gSelectedPeer is that peer's devices[]
// index, which is not the row number once this device's entry sits before it.
static void tapSelectsTheRowsPeer() {
  setDevices({1, 0xEE, 2, 3});
  showPeersMenu();
  const struct { uint32_t row; uint8_t id; int index; } taps[] = {{1, 1, 0}, {2, 2, 2}, {3, 3, 3}};
  for (const auto& tap : taps) {
    gSelectedPeer = -1;
    details = 0;
    handlePeersTap(tap.row);
    CHECK(details == 1 && gSelectedPeer == tap.index && espnow.devices[gSelectedPeer].mac[5] == tap.id);
    CHECK(!isSelfMac(espnow.devices[gSelectedPeer].mac));
  }
  // Row 0 is Back.
  details = 0;
  handlePeersTap(0);
  CHECK(details == 0 && shown.size() == 7);  // the main menu

  // The registry changed between the draw and the tap: a peer before the
  // tapped one was unpaired, so every index moved. The tap still selects the
  // peer the row showed.
  setDevices({1, 0xEE, 2, 3});
  showPeersMenu();  // rows: peer-1, peer-2, peer-3
  removeDevice(1);  // registry: this device, peer-2, peer-3
  gSelectedPeer = -1;
  details = 0;
  handlePeersTap(3);  // the row that showed peer-3
  CHECK(details == 1 && gSelectedPeer == 2 && espnow.devices[gSelectedPeer].mac[5] == 3);
  // The row of the peer that is gone: no detail page, the list drawn again as
  // it is now, and nothing selected.
  setDevices({1, 0xEE, 2, 3});
  showPeersMenu();
  removeDevice(1);
  gSelectedPeer = -1;
  details = 0;
  unsigned before = pages;
  handlePeersTap(1);
  CHECK(details == 0 && gSelectedPeer == -1 && pages == before + 1);
  CHECK(shown.size() == 3 && shown[1] == "peer-2" && shown[2] == "peer-3");

  // A tap past the rows drawn (a stale layout), or after ESP-NOW stopped.
  setDevices({0xEE, 1, 2});
  showPeersMenu();
  gSelectedPeer = -1;
  details = 0;
  handlePeersTap(3);
  handlePeersTap(16);
  handlePeersTap(4000000000u);
  CHECK(details == 0 && gSelectedPeer == -1);
  showPeersMenu();
  espnow.initialized = false;
  handlePeersTap(1);
  CHECK(details == 0 && gSelectedPeer == -1 && shown.size() == 2 && shown[1] == "(no paired peers)");

  // The empty list's "(no paired peers)" row selects nothing.
  setDevices({0xEE});
  showPeersMenu();
  handlePeersTap(1);
  CHECK(details == 0 && gSelectedPeer == -1);

  // Whatever the row table holds, this device is never selected (it used to
  // have a row whose detail page offered Ping, Send and Forget on itself).
  setDevices({0xEE, 1});
  showPeersMenu();
  std::memcpy(gPeerRowMac[0], selfMac, 6);
  handlePeersTap(1);
  CHECK(details == 0 && gSelectedPeer == -1);
}

// The lens refuses a redraw of the list (it is busy): the earlier list stays
// on the lens, so its rows keep the peers they showed, whatever the registry
// holds now. Refused on the way in from another page, no row is a peer.
static void refusedRedraw() {
  setDevices({0xEE, 1, 2});
  gSub = ESPN_APP_SUB_MAIN;
  showPeersMenu();  // on the lens: peer-1, peer-2
  removeDevice(1);
  {
    EspNowDevice& added = espnow.devices[espnow.deviceCount++];
    const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 7};
    std::memcpy(added.mac, mac, 6);  // registry: this device, peer-2, peer-7
  }
  lensRefuses = true;
  showPeersMenu();  // would draw peer-2, peer-7: refused
  lensRefuses = false;
  CHECK(shown.size() == 3 && shown[1] == "peer-1" && shown[2] == "peer-2");
  gSelectedPeer = -1;
  details = 0;
  handlePeersTap(2);  // the row still showing peer-2 (the refused list had peer-7 there)
  CHECK(details == 1 && gSelectedPeer == indexOf(2) && espnow.devices[gSelectedPeer].mac[5] == 2);

  // From the main menu, refused: the main menu is still on the lens. A tap on
  // one of its rows opens no peer; it draws the list again.
  setDevices({0xEE, 1, 2});
  showPeersMenu();
  showMainMenu();
  lensRefuses = true;
  showPeersMenu();
  lensRefuses = false;
  CHECK(gPeerRowCount == 0);
  gSelectedPeer = -1;
  details = 0;
  handlePeersTap(2);
  CHECK(details == 0 && gSelectedPeer == -1 && shown.size() == 3 && shown[1] == "peer-1" &&
        gPeerRowCount == 2);
  handlePeersTap(2);
  CHECK(details == 1 && gSelectedPeer == indexOf(2));
}

// Network > ESP-NOW > View Devices: the same peers, eight rows at most.
static void viewDevices() {
  setDevices({0xEE, 1, 2});
  espnow.devices[indexOf(1)].room.text = "Lab";
  espnow.devices[indexOf(2)].room.text = "Hall";
  espnow.devices[indexOf(2)].friendlyName.text = "Door";
  roomsPending = {2};
  showEspNowDevices();
  CHECK(netSub == NET_SUB_ESPNOW_DEVS);
  CHECK(shown.size() == 3 && shown[0] == "<- ESP-NOW" && shown[1] == "peer-1 @ Lab" && shown[2] == "Door @ Hall?");
  roomsPending.clear();
  setDevices({1, 2, 3, 4, 0xEE, 5, 6, 7, 8, 9, 10});  // ten peers: the first eight
  showEspNowDevices();
  CHECK(shown.size() == 9 && shown[1] == "peer-1" && shown[5] == "peer-5" && shown[8] == "peer-8");
  CHECK(!anyRowHas("this-device"));
  setDevices({0xEE});  // only this device: the empty list (it could never show before)
  showEspNowDevices();
  CHECK(shown.size() == 2 && shown[1].compare(0, 9, "(no peers") == 0);
}
#endif  // ENABLE_ESPNOW

int main() {
#if ENABLE_ESPNOW
  countRule();
  countRows();
  peersList();
  tapSelectsTheRowsPeer();
  refusedRedraw();
  viewDevices();
#else
  // Without ESP-NOW the count is 0/0 and the lists say so; nothing above
  // compiled a registry, a session test or a self-address test.
  int up, paired;
  counts(&up, &paired);
  CHECK(up == 0 && paired == 0);
  showMainMenu();
  CHECK(shown.size() == 7 && shown[2] == "Peers (0/0 up) >>");
  showPeersMenu();
  CHECK(shown.size() == 2 && shown[1] == "(ESP-NOW not compiled)");
  handlePeersTap(1);
  showEspNowDevices();
  CHECK(shown.size() == 2 && shown[1] == "(ESP-NOW not compiled)");
#endif
  std::printf("%u\n", checks);
  return 0;
}
