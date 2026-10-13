// Production definitions are inserted by test_ble_area_cleanup.py: the BLE
// server's connection typing (bleIdentifyDeviceByMAC and its MAC helpers),
// extracted unchanged from Bluetooth.cpp, with BLE_Types.h and the saved-target
// types from BLE_Peers.h. The peer owner's saved-target snapshot is a host
// boundary backed by a table the cases rewrite to model a re-pair.
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// INSERT_TYPES

// ---------------------------------------------------------------------------
// Host boundary: the peer owner's current saved targets (BLE_Peers.cpp).
// ---------------------------------------------------------------------------
static BlePeerSavedTarget gSavedTargets[BLE_PEER_MAX];
static unsigned gSnapshotReads = 0;

bool blePeerSavedTargetSnapshot(BlePeerKind kind, BlePeerSavedTargetSnapshot& out) {
  out = BlePeerSavedTargetSnapshot{};
  if (kind >= BLE_PEER_MAX) return false;
  ++gSnapshotReads;
  out.target = gSavedTargets[kind];
  return true;
}

static void saveTarget(BlePeerKind kind, const char* mac1, const char* mac2 = "") {
  BlePeerSavedTarget& t = gSavedTargets[kind];
  t = BlePeerSavedTarget{};
  std::snprintf(t.mac1, sizeof(t.mac1), "%s", mac1);
  std::snprintf(t.mac2, sizeof(t.mac2), "%s", mac2);
}

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static int gFailures = 0;

static void parseMac(const char* text, uint8_t out[6]) {
  unsigned v[6] = {};
  std::sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  for (int i = 0; i < 6; ++i) out[i] = (uint8_t)v[i];
}

static void expectType(const char* name, const char* mac, BLEDeviceType expected,
                       const char* why) {
  uint8_t bytes[6];
  parseMac(mac, bytes);
  const BLEDeviceType got = bleIdentifyDeviceByMAC(bytes);
  if (got != expected) {
    std::printf("FAIL [%s]: %s typed %s, expected %s (%s)\n", name, mac,
                bleDeviceTypeToString(got), bleDeviceTypeToString(expected), why);
    std::fflush(stdout);
    ++gFailures;
  }
}

// Synthetic, locally administered addresses only.
static void caseClassify() {
  saveTarget(BLE_PEER_G2_GLASSES, "02:00:00:00:0a:01", "02:00:00:00:0A:02");
  saveTarget(BLE_PEER_R1_RING, "02:00:00:00:0a:03");
  saveTarget(BLE_PEER_PHONE, "02:00:00:00:0A:04");
  const char* why = "saved peer targets, matched in any case";
  expectType("classify", "02:00:00:00:0A:01", BLE_DEVICE_GLASSES_LEFT, why);
  expectType("classify", "02:00:00:00:0A:02", BLE_DEVICE_GLASSES_RIGHT, why);
  expectType("classify", "02:00:00:00:0A:03", BLE_DEVICE_RING, why);
  expectType("classify", "02:00:00:00:0A:04", BLE_DEVICE_PHONE, why);
  expectType("classify", "02:00:00:00:0A:99", BLE_DEVICE_UNKNOWN, "address not saved for any peer");
}

// The user re-pairs the glasses and the ring and forgets the phone after the
// first connection was typed. The next connections must be typed from the
// peer owner's current targets: bledisconnect / bleDisconnectAppClients keep
// a peer link open only when it is typed as glasses or ring.
static void caseRepair() {
  saveTarget(BLE_PEER_G2_GLASSES, "02:00:00:00:0b:21", "02:00:00:00:0B:22");
  saveTarget(BLE_PEER_R1_RING, "02:00:00:00:0B:23");
  saveTarget(BLE_PEER_PHONE, "");
  const char* stale = "connection typing must follow the peer owner's current saved targets, "
                      "not a copy taken at the first connection";
  expectType("repair", "02:00:00:00:0B:21", BLE_DEVICE_GLASSES_LEFT, stale);
  expectType("repair", "02:00:00:00:0B:22", BLE_DEVICE_GLASSES_RIGHT, stale);
  expectType("repair", "02:00:00:00:0B:23", BLE_DEVICE_RING, stale);
  expectType("repair", "02:00:00:00:0A:01", BLE_DEVICE_UNKNOWN, stale);
  expectType("repair", "02:00:00:00:0A:03", BLE_DEVICE_UNKNOWN, stale);
  expectType("repair", "02:00:00:00:0A:04", BLE_DEVICE_UNKNOWN, stale);
}

// An empty saved slot never matches, including the all-zero address.
static void caseEmpty() {
  for (int k = 0; k < BLE_PEER_MAX; ++k) gSavedTargets[k] = BlePeerSavedTarget{};
  expectType("empty", "00:00:00:00:00:00", BLE_DEVICE_UNKNOWN, "no peer saved");
}

int main() {
  caseClassify();
  caseRepair();
  caseEmpty();
  if (gFailures) return 1;
  std::printf("connection typing: saved targets matched in any case, re-pair takes effect "
              "on the next connection (%u snapshot reads), empty slots never match\n",
              gSnapshotReads);
  return 0;
}
