// Host harness for espnowHistoryDisplayName (System_ESPNow.cpp): the name a
// history reader shows for a copied record. test_espnow_history_display_name.py
// extracts the production resolver and the history types from
// System_ESPNow.h into the INSERT_ markers; the mesh metadata table, the
// paired registry and GroupLockGuard are doubles here with the production
// field names and sizes (the Python side checks those sizes in the header).
// Contract under test (System_ESPNow.h): "" for a sent record; the MAC text
// for an unattributed received one; else the active mesh metadata name, then
// the paired registry name, else the MAC text; never "" for a received
// record; at most cap-1 chars + NUL; nothing written for a null out or cap 0;
// the group lock taken once (only when a lookup is needed) and released before
// return; loops bounded by MESH_PEER_MAX and the registry array; null tables.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>

#include "System_ESPNow_HistoryCore.h"
#include "System_MacText.h"

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                                                \
  do {                                                                  \
    ++gChecks;                                                          \
    if (!(cond)) {                                                      \
      ++gFailures;                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      std::fprintf(stderr, __VA_ARGS__);                                \
      std::fprintf(stderr, "\n");                                       \
    }                                                                   \
  } while (0)

// INSERT_API

// ---- doubles ------------------------------------------------------------------
struct MeshPeerMeta {
  uint8_t mac[6];
  char name[32];
  char friendlyName[48];
  bool isActive;
};

template <size_t Capacity>
struct EspNowInlineText {
  char value_[Capacity];
  static constexpr size_t capacity() { return Capacity; }
  const char* c_str() const { return value_; }
};

struct EspNowDevice {
  uint8_t mac[6];
  EspNowInlineText<32> name;
};

// devices[] is the last member so a loop past the array leaves the allocation
// (AddressSanitizer reports it).
struct EspNowState {
  int deviceCount;
  EspNowDevice devices[16];
};

static MeshPeerMeta* gMeshPeerMeta = nullptr;
static int gMeshPeerSlots = 0;
static EspNowState* gEspNow = nullptr;

static int gLockDepth = 0;
static int gLockTakes = 0;
struct GroupLockGuard {
  GroupLockGuard() { ++gLockDepth; ++gLockTakes; }
  ~GroupLockGuard() { --gLockDepth; }
  GroupLockGuard(const GroupLockGuard&) = delete;
  GroupLockGuard& operator=(const GroupLockGuard&) = delete;
};

// INSERT_RESOLVER

// ---- cases --------------------------------------------------------------------
static const uint8_t kA[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
static const uint8_t kB[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x11, 0x22};
static const char* kAText = "10:20:30:40:50:60";
static const char* kBText = "AA:BB:CC:00:11:22";

static ReceivedTextMessage rec(const uint8_t* mac, bool sent, bool unattributed) {
  ReceivedTextMessage m;
  std::memset(&m, 0, sizeof(m));
  std::memcpy(m.peerMac, mac, 6);
  m.isSent = sent;
  m.unattributed = unattributed;
  return m;
}

// Fresh tables: MESH_PEER_MAX metadata slots (heap, exact size) and a registry.
struct World {
  MeshPeerMeta* meta;
  EspNowState* state;
  World() {
    meta = new MeshPeerMeta[MESH_PEER_MAX];
    std::memset(static_cast<void*>(meta), 0, sizeof(MeshPeerMeta) * MESH_PEER_MAX);
    state = new EspNowState;
    std::memset(static_cast<void*>(state), 0, sizeof(EspNowState));
    gMeshPeerMeta = meta;
    gMeshPeerSlots = MESH_PEER_MAX;
    gEspNow = state;
  }
  ~World() {
    delete[] meta;
    delete state;
    gMeshPeerMeta = nullptr;
    gMeshPeerSlots = 0;
    gEspNow = nullptr;
  }
  void addMeta(int slot, const uint8_t* mac, const char* name, bool active = true) {
    std::memcpy(meta[slot].mac, mac, 6);
    std::strncpy(meta[slot].name, name, sizeof(meta[slot].name));
    meta[slot].isActive = active;
  }
  void addDevice(const uint8_t* mac, const char* name) {
    EspNowDevice& d = state->devices[state->deviceCount++];
    std::memcpy(d.mac, mac, 6);
    std::strncpy(d.name.value_, name, sizeof(d.name.value_));
  }
};

static std::string_view nameOf(const ReceivedTextMessage& m, size_t cap = 64) {
  static char out[64];
  std::memset(out, 'x', sizeof(out));
  gLockTakes = 0;
  espnowHistoryDisplayName(m, out, cap);
  return std::string_view(out);
}

static void caseOrder() {
  World w;
  w.addMeta(3, kA, "MetaA");
  w.addDevice(kA, "RegA");
  w.addDevice(kB, "RegB");
  ReceivedTextMessage a = rec(kA, false, false);
  CHECK(nameOf(a) == "MetaA", "meta first: %s", nameOf(a).data());
  CHECK(gLockTakes == 1 && gLockDepth == 0, "lock taken once and released: takes %d depth %d", gLockTakes,
        gLockDepth);
  ReceivedTextMessage b = rec(kB, false, false);
  CHECK(nameOf(b) == "RegB", "registry when no meta: %s", nameOf(b).data());
  // Active meta slot with an empty name: the registry name (getEspNowDeviceName's order).
  w.meta[3].name[0] = '\0';
  CHECK(nameOf(a) == "RegA", "empty meta name falls to registry: %s", nameOf(a).data());
  // Inactive meta slot is ignored.
  w.addMeta(3, kA, "Stale", false);
  CHECK(nameOf(a) == "RegA", "inactive meta ignored: %s", nameOf(a).data());
  // A rename shows at once (names are looked up when shown).
  w.addMeta(5, kA, "Renamed");
  CHECK(nameOf(a) == "Renamed", "current name: %s", nameOf(a).data());
  // Registry entry with an empty name, no meta: the MAC text, never "".
  std::memset(w.state->devices[1].name.value_, 0, 32);
  CHECK(nameOf(b) == kBText, "empty registry name -> MAC text: %s", nameOf(b).data());
  // Unknown peer: the MAC text.
  const uint8_t kC[6] = {1, 2, 3, 4, 5, 6};
  ReceivedTextMessage c = rec(kC, false, false);
  CHECK(nameOf(c) == "01:02:03:04:05:06", "unknown peer -> MAC text: %s", nameOf(c).data());
}

static void caseKinds() {
  World w;
  w.addMeta(0, kA, "MetaA");
  w.addDevice(kA, "RegA");
  ReceivedTextMessage sent = rec(kA, true, false);
  CHECK(nameOf(sent).empty(), "sent -> \"\": %s", nameOf(sent).data());
  CHECK(gLockTakes == 0, "sent takes no lock: %d", gLockTakes);
  ReceivedTextMessage sentU = rec(kA, true, true);
  CHECK(nameOf(sentU).empty(), "sent (unattributed flag) -> \"\"");
  ReceivedTextMessage un = rec(kA, false, true);
  CHECK(nameOf(un) == kAText, "unattributed -> MAC text: %s", nameOf(un).data());
  CHECK(gLockTakes == 0, "unattributed takes no lock: %d", gLockTakes);
  CHECK(gLockDepth == 0, "lock balanced");
}

static void caseUnterminated() {
  World w;
  std::memcpy(w.meta[0].mac, kA, 6);
  std::memset(w.meta[0].name, 'M', sizeof(w.meta[0].name));  // 32 bytes, no NUL
  std::memset(w.meta[0].friendlyName, 'F', sizeof(w.meta[0].friendlyName));
  w.meta[0].isActive = true;
  ReceivedTextMessage a = rec(kA, false, false);
  std::string_view got = nameOf(a);
  CHECK(got.size() == ESPNOW_HISTORY_NAME_CAP - 1 && got.find('F') == std::string_view::npos,
        "unterminated meta name bounded: %zu chars", got.size());
  // Registry name filling the whole inline buffer (cannot happen through
  // EspNowInlineText::assign, still bounded here).
  w.meta[0].isActive = false;
  w.addDevice(kA, "");
  std::memset(w.state->devices[0].name.value_, 'R', 32);
  got = nameOf(a);
  CHECK(got.size() == ESPNOW_HISTORY_NAME_CAP - 1 && got.find_first_not_of('R') == std::string_view::npos,
        "unterminated registry name bounded: %zu chars", got.size());
}

static void caseCaps() {
  World w;
  w.addMeta(0, kA, "Alpha");
  ReceivedTextMessage a = rec(kA, false, false);
  ReceivedTextMessage un = rec(kA, false, true);
  char out[8];
  std::memset(out, '#', sizeof(out));
  espnowHistoryDisplayName(a, out, 0);
  CHECK(out[0] == '#', "cap 0 writes nothing");
  espnowHistoryDisplayName(a, nullptr, 8);  // must not crash
  espnowHistoryDisplayName(a, out, 1);
  CHECK(out[0] == '\0' && out[1] == '#', "cap 1 -> \"\"");
  std::memset(out, '#', sizeof(out));
  espnowHistoryDisplayName(a, out, 2);
  CHECK(out[0] == 'A' && out[1] == '\0' && out[2] == '#', "cap 2 -> one char");
  std::memset(out, '#', sizeof(out));
  espnowHistoryDisplayName(un, out, 3);
  CHECK(std::strcmp(out, "10") == 0 && out[3] == '#', "cap 3 MAC text -> \"10\": %s", out);
  std::memset(out, '#', sizeof(out));
  espnowHistoryDisplayName(a, out, sizeof(out));
  CHECK(std::strcmp(out, "Alpha") == 0, "cap 8: %s", out);
  CHECK(gLockDepth == 0, "lock balanced");
}

static void caseNullTables() {
  gMeshPeerMeta = nullptr;
  gMeshPeerSlots = 0;
  gEspNow = nullptr;
  ReceivedTextMessage a = rec(kA, false, false);
  CHECK(nameOf(a) == kAText, "no tables -> MAC text: %s", nameOf(a).data());
  {
    World w;  // ESP-NOW closed: state present, nothing paired, no meta
    w.state->deviceCount = 0;
    CHECK(nameOf(a) == kAText, "closed -> MAC text: %s", nameOf(a).data());
    gMeshPeerMeta = nullptr;
    w.addDevice(kA, "RegA");
    CHECK(nameOf(a) == "RegA", "no meta table -> registry: %s", nameOf(a).data());
    gMeshPeerMeta = w.meta;
    gEspNow = nullptr;
    w.addMeta(1, kA, "MetaA");
    CHECK(nameOf(a) == "MetaA", "no registry -> meta: %s", nameOf(a).data());
    gEspNow = w.state;
  }
  CHECK(gLockDepth == 0, "lock balanced");
}

static void caseBounds() {
  World w;
  // Counts past the arrays: the loops stop at MESH_PEER_MAX and 16 (ASan
  // reports a read past either allocation).
  gMeshPeerSlots = MESH_PEER_MAX + 50;
  w.state->deviceCount = 1000;
  ReceivedTextMessage a = rec(kA, false, false);
  CHECK(nameOf(a) == kAText, "oversized counts -> MAC text: %s", nameOf(a).data());
  w.addMeta(MESH_PEER_MAX - 1, kA, "LastSlot");
  CHECK(nameOf(a) == "LastSlot", "last meta slot found: %s", nameOf(a).data());
  w.meta[MESH_PEER_MAX - 1].isActive = false;
  std::memcpy(w.state->devices[15].mac, kA, 6);
  std::strncpy(w.state->devices[15].name.value_, "LastDev", 32);
  CHECK(nameOf(a) == "LastDev", "last registry entry found: %s", nameOf(a).data());
  // Fewer configured slots than MESH_PEER_MAX: slots beyond are not read.
  gMeshPeerSlots = 2;
  w.addMeta(5, kA, "Beyond");
  CHECK(nameOf(a) == "LastDev", "slot beyond gMeshPeerSlots ignored: %s", nameOf(a).data());
}

// The espnowmessages page (espnowHistoryPageNames): one lookup per distinct
// attributed peer; an unattributed or sent row never lends its name to an
// attributed one, and another peer's name is never reused.
static void casePage() {
  World w;
  w.addMeta(0, kA, "MetaA");
  w.addDevice(kB, "RegB");
  ReceivedTextMessage page[7] = {
      rec(kA, false, true),   // unattributed A: MAC text
      rec(kA, false, false),  // attributed A: looked up, not the row above's MAC text
      rec(kB, false, false),  // attributed B: looked up, not A's name
      rec(kA, true, false),   // sent: ""
      rec(kA, false, false),  // attributed A again: reuses row 1
      rec(kB, false, true),   // unattributed B: MAC text, not row 2's name
      rec(kB, false, false),  // attributed B again: reuses row 2
  };
  char names[7][ESPNOW_HISTORY_NAME_CAP];
  std::memset(names, 'x', sizeof(names));
  gLockTakes = 0;
  espnowHistoryPageNames(page, 7, names);
  const char* want[7] = {kAText, "MetaA", "RegB", "", "MetaA", kBText, "RegB"};
  for (int i = 0; i < 7; i++) CHECK(std::strcmp(names[i], want[i]) == 0, "row %d: '%s', want '%s'", i, names[i], want[i]);
  CHECK(gLockTakes == 2 && gLockDepth == 0, "one lookup per attributed peer: %d lock takes", gLockTakes);
  // A rename between pages shows on the next page (nothing is kept between calls).
  w.addMeta(0, kA, "NewA");
  espnowHistoryPageNames(page + 1, 1, names);
  CHECK(std::strcmp(names[0], "NewA") == 0, "next page: '%s'", names[0]);
  // n 0 and null tables write nothing.
  std::memset(names, 'x', sizeof(names));
  espnowHistoryPageNames(page, 0, names);
  espnowHistoryPageNames(nullptr, 3, names);
  espnowHistoryPageNames(page, 3, nullptr);
  CHECK(names[0][0] == 'x', "nothing written for an empty page");
}

int main(int argc, char** argv) {
  struct { const char* name; void (*fn)(); } cases[] = {
    {"order", caseOrder}, {"kinds", caseKinds}, {"unterminated", caseUnterminated},
    {"caps", caseCaps}, {"null_tables", caseNullTables}, {"bounds", caseBounds}, {"page", casePage},
  };
  for (auto& c : cases) {
    if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
    const int before = gFailures;
    c.fn();
    std::printf("CASE %s %s\n", c.name, gFailures == before ? "ok" : "FAILED");
  }
  std::printf("espnow_history_display_name: %d checks, %d failures\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
