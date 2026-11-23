// Host boundaries around the lens "Peers" rows, compiled with the production
// code test_g2_espnow_peer_rows.py lifts from G2_Page_Network.cpp
// (g2EspNowPeerCounts, g2FormatPeerCounts, g2BuildNetworkInfo,
// showEspNowDevices), G2_Page_ESPNow.cpp (the row table, g2BuildEspNowAppInfo,
// showMainMenu, showPeersMenu, showStatsMenu, handlePeersTap) and
// System_ESPNow.h (espnowPeerState, espnowPeerCounts: the peer-state rule of
// System_ESPNow_PeerStateCore.h). The ESP-NOW registry, the heartbeat table,
// the session test, the self-address test and the lens list page are doubles.
// Built twice: ENABLE_ESPNOW=1 (the cases below) and ENABLE_ESPNOW=0, where no
// registry, heartbeat, session or self-address double exists, so the lifted
// code may not name one outside its #if.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_MacText.h"  // production macToDisplay (the list's last-resort label)
#include "System_ESPNow_PeerStateCore.h"  // production: online / connecting / offline

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
// Lens notice boundary (G2_HijackCmd.h): no refusal is pending here, so each
// back row keeps its label.
static constexpr int G2_HIJACK_PAGE_ESPNOW_APP = 10;
static void g2LensNoticeBackRow(int, const char* label, char* out, size_t cap) {
  if (label != out) std::snprintf(out, cap, "%s", label);
}

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
// The heartbeat table: a slot per peer heard, by the last address byte, and
// whether it was heard within the 30 s window (meshPeerLinkOf).
struct MeshPeerHealth { uint8_t id; bool recent; };
static std::vector<MeshPeerHealth> heartbeats;
static MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing) {
  CHECK(!createIfMissing);  // a read never creates a slot
  CHECK(std::memcmp(mac, selfMac, 6) != 0);
  for (auto& slot : heartbeats) if (slot.id == mac[5]) return &slot;
  return nullptr;
}
// The link (1.0 step 8): heard and proven both ways here (one-way is covered by
// test_espnow_peer_state_surfaces and espnow_link_core_tests).
static MeshLink meshPeerLinkOf(const MeshPeerHealth* slot) {
  return slot && slot->recent ? MeshLink::TwoWay : MeshLink::None;
}
static void heard(std::initializer_list<uint8_t> ids) {  // recent heartbeats from these peers only
  heartbeats.clear();
  for (uint8_t id : ids) heartbeats.push_back({id, true});
}

// INSERT_PEER_STATE
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
static bool same(const EspNowPeerCounts& peers, int online, int connecting, int offline) {
  return peers.online == online && peers.connecting == connecting && peers.offline == offline;
}
static std::string formatted(const EspNowPeerCounts& peers) {
  char out[24];
  std::memset(out, 'x', sizeof(out));
  g2FormatPeerCounts(peers, out, sizeof(out));
  return out;
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

// The count: every registry entry except this device is a peer. One is
// online with a recent heartbeat AND an ACTIVE session, connecting with the
// heartbeat alone, offline without a recent heartbeat (a session that outlived
// the heartbeats does not make it online). The rule of every surface.
static void countRule() {
  espnow = EspNowState();  // not running
  heard({1, 2});
  sessionsUp = {1, 2};
  CHECK(same(g2EspNowPeerCounts(), 0, 0, 0));
  setDevices({0xEE, 1, 2, 3});
  heard({1, 2, 3});
  sessionsUp = {1, 2};
  CHECK(same(g2EspNowPeerCounts(), 2, 1, 0) && g2EspNowPeerCounts().paired() == 3);
  setDevices({1, 2, 0xEE, 3});  // this device's entry anywhere in the registry
  sessionsUp = {3};
  CHECK(same(g2EspNowPeerCounts(), 1, 2, 0));
  // Connecting is never online: heartbeats without sessions count none.
  sessionsUp.clear();
  CHECK(same(g2EspNowPeerCounts(), 0, 3, 0));
  // A session without a recent heartbeat is offline, not online.
  heard({1});
  sessionsUp = {1, 2, 3};
  CHECK(same(g2EspNowPeerCounts(), 1, 0, 2));
  heartbeats = {{1, true}, {2, false}, {3, false}};  // slots whose heartbeat timed out
  CHECK(same(g2EspNowPeerCounts(), 1, 0, 2));
  // Unpaired peers (heard, session up, not in the registry) are not counted.
  setDevices({0xEE, 1});
  heard({1, 4, 5});
  sessionsUp = {1, 4, 5};
  CHECK(same(g2EspNowPeerCounts(), 1, 0, 0));
  setDevices({0xEE});  // only this device: no peer
  CHECK(same(g2EspNowPeerCounts(), 0, 0, 0));
  setDevices({1, 2});  // no own entry yet (name not applied)
  heard({1, 2});
  sessionsUp = {1, 2};
  CHECK(same(g2EspNowPeerCounts(), 2, 0, 0));
  // A count past the 16 entries the registry holds reads 16, never beyond.
  setDevices({0xEE, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
  espnow.deviceCount = 40;
  heard({14, 15});
  sessionsUp = {15};
  CHECK(same(g2EspNowPeerCounts(), 1, 1, 13));
  // Stopped: nothing is counted from a registry that is still filled.
  espnow.initialized = false;
  CHECK(same(g2EspNowPeerCounts(), 0, 0, 0));

  // The lens wording: online/paired "up", connecting appended, whole in 24 bytes.
  EspNowPeerCounts peers;
  CHECK(formatted(peers) == "0/0 up");
  peers.online = 2; peers.offline = 1;
  CHECK(formatted(peers) == "2/3 up");
  peers.connecting = 1;
  CHECK(formatted(peers) == "2/4 up +1 conn");
  peers.online = 16; peers.connecting = 16; peers.offline = 0;
  CHECK(formatted(peers) == "16/32 up +16 conn");
  char small[8];
  g2FormatPeerCounts(peers, small, sizeof(small));
  CHECK(std::strlen(small) == 7);  // cut, terminated
  g2FormatPeerCounts(peers, nullptr, 8);
  small[0] = 'x';
  g2FormatPeerCounts(peers, small, 0);
  CHECK(small[0] == 'x');
}

// The five rows that print the count show online/paired (and the connecting
// peers when there are some), and none shows the registry size (4 here) or
// the peers with message history (9).
static void countRows() {
  setDevices({0xEE, 1, 2, 3});
  heard({1, 2, 3});
  sessionsUp = {1, 3};
  showMainMenu();
  CHECK(shown.size() == 7 && shown[2] == "Peers (2/3 up +1 conn) >>");
  showStatsMenu();
  CHECK(anyRowHas("Peers: 2/3 up +1 conn"));
  CHECK(textOf(g2BuildEspNowAppInfo).find("Peers: 2/3 up +1 conn\n") != std::string::npos);
  CHECK(textOf(g2BuildNetworkInfo).find("ESPNow mesh\nPeers 2/3 up +1 conn\n") != std::string::npos);
  heard({1, 3});  // peer 2 went silent: offline, no longer connecting
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
  heard({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
  sessionsUp = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  showMainMenu();
  CHECK(shown[2] == "Peers (15/15 up) >>");
  showStatsMenu();
  CHECK(anyRowHas("Peers: 15/15 up"));
  sessionsUp = {1, 2, 3, 4, 5, 6, 7};
  showMainMenu();
  CHECK(shown[2] == "Peers (7/15 up +8 conn) >>");
  showStatsMenu();
  CHECK(anyRowHas("Peers: 7/15 up +8 conn"));

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
  CHECK(g2EspNowPeerCounts().paired() == int(shown.size()) - 1);
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
  CHECK(same(g2EspNowPeerCounts(), 0, 0, 0));
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
