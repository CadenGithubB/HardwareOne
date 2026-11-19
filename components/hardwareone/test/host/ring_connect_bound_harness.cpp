// The ring's direct connect bound and its failed-connect handling (G2_Ring.cpp:
// RING_DIRECT_CONNECT_TIMEOUT_MS, its static_assert against Bluedroid's
// CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT, ringSettleClientAfterFailedConnect) over
// a fake BLEClient outcome. test_ring_connect_bound.py inserts the production
// text. An OPEN_TIMEOUT whose GATT application unregistered cleanly (our bound
// ending an attempt to an absent ring) gives the link slot back and keeps the
// client; nothing else changes path, and none of it reaches the temples.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static int gChecks = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    ++gChecks;                                                                \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x);       \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

#ifndef CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT
#define CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT 30
#endif

enum class BLEClientConnectStage : uint8_t {
  NONE = 0, PRECHECK, APP_REGISTER_API, APP_REGISTER_EVENT, APP_REGISTER_TIMEOUT,
  OPEN_API, OPEN_EVENT, OPEN_TIMEOUT, COMPLETE,
};
enum class BLEClientUnregisterState : uint8_t {
  NOT_REQUESTED = 0, REQUESTED, COMPLETED, REQUEST_FAILED, COMPLETION_TIMEOUT, COMPLETED_LATE,
};
struct BLEClientConnectOutcome {
  BLEClientConnectStage stage = BLEClientConnectStage::NONE;
  BLEClientUnregisterState unregisterState = BLEClientUnregisterState::NOT_REQUESTED;
  bool success = false;
};
class BLEClient {
 public:
  BLEClientConnectOutcome outcome;
  BLEClientConnectOutcome getLastConnectOutcome() const { return outcome; }
};

enum BlePeerKind : uint8_t { BLE_PEER_G2_GLASSES = 0, BLE_PEER_R1_RING = 1 };
static constexpr uint8_t BLE_CLIENT_RETIRE_R1 = 2;

static struct { BLEClient* client = nullptr; } gRing;

// What the shared helpers were asked to do. A recycle would be armed only
// inside bleQuarantineClientAfterFailedConnect (G2_Glasses.cpp), so a release
// with no quarantine call means no host recycle and no temple touched.
static int gReleases = 0, gQuarantines = 0;
static const void* gReleased = nullptr;
static void bleCentralLinkReleaseClient(const void* client) { ++gReleases; gReleased = client; }
static bool bleQuarantineClientAfterFailedConnect(BLEClient*& client, uint8_t slot, uint8_t peer,
                                                  const char* tag) {
  CHECK(client && slot == BLE_CLIENT_RETIRE_R1 && peer == BLE_PEER_R1_RING && tag);
  ++gQuarantines;
  return true;
}

// INSERT_PRODUCTION

static void reset(BLEClientConnectStage stage, BLEClientUnregisterState unreg) {
  static BLEClient client;
  client.outcome = BLEClientConnectOutcome{};
  client.outcome.stage = stage;
  client.outcome.unregisterState = unreg;
  gRing.client = &client;
  gReleases = gQuarantines = 0;
  gReleased = nullptr;
}

int main() {
  // The bound: 25 s, under Bluedroid's 30 s link timer.
  CHECK(RING_DIRECT_CONNECT_TIMEOUT_MS == 25000);
  CHECK(RING_DIRECT_CONNECT_TIMEOUT_MS < (uint32_t)CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT * 1000u);

  // Our bound ended the attempt and the GATT app unregistered: an ordinary
  // miss. The slot goes back, the client stays for the next attempt.
  reset(BLEClientConnectStage::OPEN_TIMEOUT, BLEClientUnregisterState::COMPLETED);
  BLEClient* const kept = gRing.client;
  ringSettleClientAfterFailedConnect();
  CHECK(gReleases == 1 && gReleased == kept && gQuarantines == 0 && gRing.client == kept);

  // Every other failure keeps the shared quarantine path.
  struct Case { BLEClientConnectStage stage; BLEClientUnregisterState unreg; };
  const Case others[] = {
      {BLEClientConnectStage::OPEN_TIMEOUT, BLEClientUnregisterState::COMPLETION_TIMEOUT},
      {BLEClientConnectStage::OPEN_TIMEOUT, BLEClientUnregisterState::REQUEST_FAILED},
      {BLEClientConnectStage::OPEN_TIMEOUT, BLEClientUnregisterState::REQUESTED},
      {BLEClientConnectStage::OPEN_TIMEOUT, BLEClientUnregisterState::COMPLETED_LATE},
      {BLEClientConnectStage::OPEN_EVENT, BLEClientUnregisterState::COMPLETED},
      {BLEClientConnectStage::APP_REGISTER_TIMEOUT, BLEClientUnregisterState::COMPLETED},
      {BLEClientConnectStage::PRECHECK, BLEClientUnregisterState::NOT_REQUESTED},
      {BLEClientConnectStage::OPEN_API, BLEClientUnregisterState::COMPLETED},
  };
  for (const Case& c : others) {
    reset(c.stage, c.unreg);
    ringSettleClientAfterFailedConnect();
    CHECK(gQuarantines == 1 && gReleases == 0);
  }

  // No client: nothing to do.
  reset(BLEClientConnectStage::OPEN_TIMEOUT, BLEClientUnregisterState::COMPLETED);
  gRing.client = nullptr;
  ringSettleClientAfterFailedConnect();
  CHECK(gQuarantines == 0 && gReleases == 0);

  std::printf("Ring connect bound: %d checks; 25 s under Bluedroid's %d s timer; a clean "
              "open-timeout releases the slot and keeps the client, every other failure "
              "is quarantined\n",
              gChecks, (int)CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT);
  return 0;
}
