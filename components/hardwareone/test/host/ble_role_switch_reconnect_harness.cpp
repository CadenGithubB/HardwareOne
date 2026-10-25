// `openble` (server mode for a direct app connection) and back to the glasses
// with `openg2 saved`: what the ring, the glasses and the phone pipe do across
// the role switch. test_ble_role_switch_reconnect.py inserts the production
// code: the reconnect scheduler and tick (BLE_Peers.cpp), `openble` and
// bleSubsystemActive (Bluetooth.cpp), the ring's peer ops, admission, logical
// down, disconnect and `ring disconnect` (G2_Ring.cpp), the glasses' peer ops,
// admission, disconnect intent, `openg2` and g2ConnectSaved (G2_Glasses.cpp)
// and the phone pipe's peer ops table (BLE_PhonePipe.cpp).
//
// The role owners are fakes in the production order, which the driver's text
// checks and test_g2_client_lifecycle.py pin: initBluetooth stops the pipe,
// then deinitG2Client takes the glasses and the ring down without a user
// disconnect; the pipe's worker notes a READY link it stopped as lost; and
// initG2Client registers the glasses and ring, starts the pipe, then arms the
// peers. Single task, virtual clock.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>

#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_BLE_PHONE_PIPE 1

static long gChecks = 0;
#define CHECK(x)                                                         \
  do {                                                                   \
    ++gChecks;                                                           \
    if (!(x)) {                                                          \
      std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x);  \
      std::exit(1);                                                      \
    }                                                                    \
  } while (0)

static void quietLog(const char*, ...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
#define DEBUG_RING_LIFECYCLEF(...) quietLog(__VA_ARGS__)
#define WARN_RINGF(...) quietLog(__VA_ARGS__)
#define BLE_DEBUGF(flag, ...) quietLog(__VA_ARGS__)
#define DEBUG_BLE_CORE 0
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)

// ---- Arduino and FreeRTOS on a virtual clock --------------------------------
class String {
 public:
  String(const char* text = "") : text_(text ? text : "") {}
  unsigned length() const { return (unsigned)text_.size(); }
  const char* c_str() const { return text_.c_str(); }
  void toLowerCase() { for (char& c : text_) c = (char)std::tolower((unsigned char)c); }
  bool operator==(const char* other) const { return text_ == other; }
 private:
  std::string text_;
};
class CommandArgs {
 public:
  explicit CommandArgs(const String& args) {
    std::string word;
    for (const char* p = args.c_str();; ++p) {
      if (*p && *p != ' ') { word += *p; continue; }
      if (!word.empty()) words_.push_back(word);
      word.clear();
      if (!*p) break;
    }
  }
  String arg(size_t i) const { return i < words_.size() ? String(words_[i].c_str()) : String(); }
 private:
  std::vector<std::string> words_;
};
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(ms) (ms)
using SemaphoreHandle_t = void*;
constexpr int pdTRUE = 1;
static int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return pdTRUE; }
static int xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
static uint32_t gNow = 1000;
static uint32_t millis() { return gNow; }
static void vTaskDelay(uint32_t ticks) { gNow += ticks; }

// INSERT_TYPES

// ---- Who owns the radio -----------------------------------------------------
static struct World {
  bool server = false, client = false, pipeActive = false;
  bool failServerStart = false;  // initBluetooth refuses before any teardown
  bool failServerHost = false;   // initBluetooth closes the client side, then its host start fails
  unsigned clientStarts = 0, serverStarts = 0;
} world;
static bool isBleServerInitialized() { return world.server; }
static bool isG2ClientInitialized() { return world.client; }
static bool phonePipeActive() { return world.pipeActive; }
// The shared client stack (BLE_CentralStack.cpp): up with the glasses runtime here.
static bool bleStackReady() { return world.client; }
bool bleSubsystemActive();
// One task here: no other role transition is ever in progress.
static BleRoleTransition bleRoleTransitionState() { return BleRoleTransition::IDLE; }
static bool bleRoleTransitionHeldByCurrentTask() { return false; }
static bool bleCentralRingShouldYield() { return false; }  // a four-link C6
static struct Settings { bool bleEnabled = true; int bleMode = BLE_MODE_G2_CLIENT; } gSettings;
static bool setSetting(int& field, int value) { field = value; return true; }

// INSERT_PEERS

// ---- Connect jobs handed to the setup worker --------------------------------
struct Job { BleConnectKind kind; bool autoReconnect, explicitReseek; uint32_t at; };
static std::vector<Job> gJobs;
bool g2SubmitBleConnect(const BleConnectJob& job) {
  gJobs.push_back({job.kind, job.peerAutoReconnect, job.peerExplicitReseek, gNow});
  return true;
}
static unsigned jobs(BleConnectKind kind) {
  unsigned n = 0;
  for (const Job& job : gJobs) n += job.kind == kind;
  return n;
}
static const Job* lastJob(BleConnectKind kind) {
  for (auto it = gJobs.rbegin(); it != gJobs.rend(); ++it) if (it->kind == kind) return &*it;
  return nullptr;
}

// ---- Ring boundary ------------------------------------------------------------
class BLERemoteCharacteristic;
class BLEClient {
 public:
  bool connected = false;
  unsigned disconnects = 0;
  bool isConnected() { return connected; }
  void disconnect() { connected = false; ++disconnects; }
};
struct RingCompletionGuard { explicit operator bool() const { return true; } };
enum SysEvt { SYSEVT_RING_DISCONNECTED, SYSEVT_RING_RECONNECT_FAILED, SYSEVT_G2_DISCONNECTED };
static void systemEventPost(SysEvt, const char*, const char*) {}
static String gRingDeviceName, gRingDeviceAddress;
static uint8_t sRingWearPosted = 0;
static void ringTransportDisconnected() {}
static void ringPushStatusEvent(const char*) {}
static void ringClearGattPointers(bool) {}
static void ringNoteConnectFailure(const char*, uint32_t) {}
static void g2RingWithdrawQueuedConnect() {}
static void g2CancelActiveRingScan() {}
static bool bleCentralTxTake(uint32_t) { return true; }
static void bleCentralTxGive() {}
static bool g2RingConnectSaved() { return false; }  // legacy op; the tick uses admission
void g2RingDisconnect(bool userInitiated = false);
// The ring owner as a device on the shared stack: open once g2RingInit ran (or
// the link came up); `closering` stamps the user's intent through the spliced
// g2RingDisconnect and closes it. The admission thunk opens a closed owner.
static bool gRingOpen = false;
static bool g2RingInit();
static bool g2RingIsOpen() { return gRingOpen; }
static bool g2RingClose(bool userInitiated) { g2RingDisconnect(userInitiated); gRingOpen = false; return true; }

// INSERT_RING

static unsigned gRingInits = 0;
static bool g2RingInit() { ++gRingInits; gRingOpen = true; return bleRegisterPeer(ringPeerSpec); }

// ---- Glasses boundary ---------------------------------------------------------
struct G2CompletionGuard { explicit operator bool() const { return true; } };
static uint32_t gG2CancelEpoch = 1;
static uint32_t bleConnectCancelAdvance() { return ++gG2CancelEpoch; }
static volatile bool gConnectTaskActive = false;
static bool g2ConnectClaim(uint32_t* cancelEpochOut) {
  if (gConnectTaskActive) return false;
  gConnectTaskActive = true;
  if (cancelEpochOut) *cancelEpochOut = gG2CancelEpoch;
  return true;
}
static void g2ConnectRelease() { gConnectTaskActive = false; }
static unsigned gStamps = 0;
static void bleStampPairedByIfBlank(BlePeerKind) { ++gStamps; }
static bool g2Connect(G2Eye) { return false; }
bool initG2Client();
bool g2ConnectSaved();
bool g2Disconnect(bool userInitiated = false);

// INSERT_GLASSES

// The production body (test_ring_phone_yield.py runs it) publishes the
// logical down first, from this very argument; the temples' GATT teardown that
// follows is not modelled.
bool g2Disconnect(bool userInitiated) {
  g2PublishDisconnectIntent(userInitiated);
  return true;
}

// ---- Phone pipe ---------------------------------------------------------------
static bool gPhoneReady = false;
bool phonePipeReady() { return gPhoneReady; }
namespace pipe {
static unsigned admits = 0;
static BlePeerConnectRequest last;
static BlePeerConnectAdmission admit(const BlePeerConnectRequest& request) {
  if (!world.pipeActive) return BlePeerConnectAdmission::ROLE_BLOCKED;
  if (gPhoneReady) return BlePeerConnectAdmission::ALREADY_UP;
  ++admits; last = request;
  return BlePeerConnectAdmission::STARTED;
}
static void disconnect() {}
// INSERT_PIPE_SPEC
}
static bool phonePipeStart(bool) {
  world.pipeActive = true;
  (void)bleRegisterPeer(pipe::spec);
  return true;
}
// Stopping ends a READY link's job; the worker then notes the link lost.
static bool phonePipeStop() {
  if (!world.pipeActive) return true;
  const bool lost = gPhoneReady;
  gPhoneReady = false;
  world.pipeActive = false;
  if (lost) blePeerNoteLinkLost(BLE_PEER_PHONE);
  return true;
}

// ---- Role owners --------------------------------------------------------------
static bool deinitG2Client() {
  if (!world.client) return true;
  if (!phonePipeStop()) return false;
  (void)g2Disconnect();
  g2RingDisconnect();
  world.client = false;
  return true;
}
bool initBluetooth() {
  ++world.serverStarts;
  if (world.failServerStart) return false;
  if (!phonePipeStop()) return false;
  if (!deinitG2Client()) return false;
  if (!g2RingClose(/*userInitiated=*/false)) return false;  // the ring owner, as production closes it
  if (world.failServerHost) return false;
  world.server = true;
  return true;
}
static bool startBLEAdvertising() { return true; }
struct PollPauseGuard {};
bool initG2Client() {
  ++world.clientStarts;
  if (world.client) return true;
  world.server = false;  // deinitBluetooth
  world.client = true;
  (void)bleRegisterPeer(g2PeerSpec);
  (void)g2RingInit();
  (void)phonePipeStart(false);
  bleArmAutoReconnectPeers();
  return true;
}

// The shared client stack on its own (BLE_CentralStack.cpp bleStackUp): here
// the glasses runtime is its only tenant, so it comes back under the same flag;
// it registers nothing (the peers stay registered) and re-arms auto-reconnect.
static bool bleStackUp() {
  if (world.client) return true;
  ++world.clientStarts;
  world.server = false;
  world.client = true;
  bleArmAutoReconnectPeers();
  return true;
}

// INSERT_BLUETOOTH

// ---- Scenario helpers ---------------------------------------------------------
static BLEClient gRingClient;
static void reset() {
  for (auto& state : sReconnect) state = PeerReconnectRuntime{};
  for (auto& peer : gPeerByKind) peer = nullptr;
  for (auto& peer : gPeerInOrder) peer = nullptr;
  gPeerCount = 0;
  world = World{};
  gSettings = Settings{};
  gJobs.clear();
  gRing = G2RingState{};
  gRingOpen = false;
  gRingClient = BLEClient{};
  gRingUpEventPublished = false;
  gRingConnectTaskActive = false;
  gRingDisconnecting = false;
  gG2PublishedTempleMask = 0;
  gConnectTaskActive = false;
  gPhoneReady = false;
  pipe::admits = 0;
  gNow += 3600000;
}
// The persisted record: saved target and owner authority, as settings load
// publishes them; auto-reconnect as given.
static void enroll(BlePeerKind kind, bool autoReconnect) {
  PeerReconnectRuntime& state = sReconnect[kind];
  state = PeerReconnectRuntime{};
  state.autoReconnect = autoReconnect;
  state.hasSavedTarget = state.ownerAuthorityAvailable = true;
  state.intentGeneration = state.identityGeneration = state.ownerGeneration = 1;
  std::strcpy(state.savedTarget.mac1, kind == BLE_PEER_PHONE ? "app-key-v1" : "aa:bb:cc:dd:ee:01");
  if (kind == BLE_PEER_G2_GLASSES) std::strcpy(state.savedTarget.mac2, "aa:bb:cc:dd:ee:02");
}
static void glassesModeBoot() {
  bleRegisterBuiltinPeers();
  gSettings.bleMode = BLE_MODE_G2_CLIENT;
  CHECK(initG2Client() && world.client && world.pipeActive);
}
static void glassesUp() {
  gG2PublishedTempleMask = 0x03;
  blePeerNoteLinkUp(BLE_PEER_G2_GLASSES);
}
static void ringUp() {
  gRingOpen = true;
  gRing.client = &gRingClient;
  gRingClient.connected = true;
  gRing.connected = true;
  gRingUpEventPublished = true;
  g2RingConnectMarkComplete();
  blePeerNoteLinkUp(BLE_PEER_R1_RING);
}
static void phoneUp() {
  gPhoneReady = true;
  blePeerNoteLinkUp(BLE_PEER_PHONE);
}
static void tick(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 1000) {
    gNow += 1000;
    bleAutoReconnectTick();
  }
}
static void openconsole() {
  CHECK(std::strcmp(cmd_openconsole(String()),
                    "App console server started and advertising (glasses, ring and phone closed until 'openble')") == 0);
  CHECK(world.server && !world.client && !world.pipeActive);
  CHECK(gSettings.bleMode == BLE_MODE_SERVER);
}
static void openg2Saved() {
  CHECK(std::strstr(cmd_g2connect(String("saved")), "saved-MAC connect started") != nullptr);
  CHECK(world.client && !world.server && world.pipeActive);
  CHECK(gSettings.bleMode == BLE_MODE_G2_CLIENT);
}
// Ten minutes of server mode: the tick holds every intent and starts nothing,
// no client host included (no role flip back).
static void serverMinutes() {
  const size_t jobsBefore = gJobs.size();
  const unsigned admitsBefore = pipe::admits, clientStarts = world.clientStarts;
  tick(10 * 60 * 1000);
  CHECK(gJobs.size() == jobsBefore && pipe::admits == admitsBefore);
  CHECK(world.clientStarts == clientStarts && world.server && !world.client);
}

int main() {
  // The reported case: glasses, ring and phone up; openble; openg2 saved.
  // The ring (auto-reconnect on) comes back, and so does the phone pipe.
  {
    reset();
    enroll(BLE_PEER_G2_GLASSES, false);
    enroll(BLE_PEER_R1_RING, true);
    enroll(BLE_PEER_PHONE, true);
    glassesModeBoot();
    glassesUp(); ringUp(); phoneUp();
    openconsole();
    for (BlePeerKind kind : {BLE_PEER_G2_GLASSES, BLE_PEER_R1_RING, BLE_PEER_PHONE}) {
      CHECK(!sReconnect[kind].userDisconnect);   // a role switch is no user disconnect
      CHECK(sReconnect[kind].wantReconnect);
    }
    serverMinutes();
    CHECK(sReconnect[BLE_PEER_R1_RING].wantReconnect && !sReconnect[BLE_PEER_R1_RING].userDisconnect);
    const unsigned admits = pipe::admits;
    const uint32_t back = gNow;
    openg2Saved();
    CHECK(jobs(BleConnectKind::G2_SAVED) == 1);
    tick(20000);
    CHECK(jobs(BleConnectKind::RING_SAVED) == 1);
    CHECK(lastJob(BleConnectKind::RING_SAVED)->autoReconnect);
    CHECK(lastJob(BleConnectKind::RING_SAVED)->at - back <= 15000);
    CHECK(pipe::admits > admits && pipe::last.autoReconnect);
    CHECK(jobs(BleConnectKind::G2_SAVED) == 1);  // openg2's own job; no second one
    std::puts("openble then openg2 saved: ring and phone pipe come back");
  }
  // A ring the user disconnected stays down, whether before openble or while
  // the server owns the radio; the glasses and the phone pipe still come back.
  for (bool duringServer : {false, true}) {
    reset();
    enroll(BLE_PEER_G2_GLASSES, true);
    enroll(BLE_PEER_R1_RING, true);
    enroll(BLE_PEER_PHONE, true);
    glassesModeBoot();
    glassesUp(); ringUp(); phoneUp();
    static const char* const kRingClosed =
        "RING: closed (link dropped, owner task released; glasses, phone and Bluetooth stay up)";
    if (!duringServer) CHECK(std::strcmp(cmd_closering(String()), kRingClosed) == 0);
    openconsole();
    // The server switch closed the ring without the user's stamp; a user's
    // closering on the closed owner says so and still stamps the intent.
    if (duringServer) CHECK(std::strcmp(cmd_closering(String()),
                                        "RING: already closed (auto-reconnect stays quiet until openring)") == 0);
    CHECK(sReconnect[BLE_PEER_R1_RING].userDisconnect && !sReconnect[BLE_PEER_R1_RING].wantReconnect);
    serverMinutes();
    const unsigned admits = pipe::admits;
    openg2Saved();
    tick(10 * 60 * 1000);
    CHECK(jobs(BleConnectKind::RING_SAVED) == 0);
    CHECK(jobs(BleConnectKind::G2_SAVED) >= 1 && pipe::admits > admits);
    std::printf("ring disconnected by the user %s openble stays down\n", duringServer ? "after" : "before");
  }
  // Auto-reconnect off: a ring that was up comes back once with the client
  // host; one that was already down does not.
  for (bool wasUp : {true, false}) {
    reset();
    enroll(BLE_PEER_G2_GLASSES, false);
    enroll(BLE_PEER_R1_RING, false);
    glassesModeBoot();
    glassesUp();
    if (wasUp) ringUp();
    openconsole();
    serverMinutes();
    openg2Saved();
    tick(20000);
    CHECK(jobs(BleConnectKind::RING_SAVED) == (wasUp ? 1U : 0U));
    if (wasUp) {
      CHECK(!lastJob(BleConnectKind::RING_SAVED)->autoReconnect &&
            lastJob(BleConnectKind::RING_SAVED)->explicitReseek);
      g2RingConnectMarkComplete();  // that attempt did not connect
    }
    tick(10 * 60 * 1000);
    CHECK(jobs(BleConnectKind::RING_SAVED) == (wasUp ? 1U : 0U));
    std::printf("auto-reconnect off, ring %s before openble: %s\n", wasUp ? "up" : "down",
                wasUp ? "back once" : "stays down");
  }
  // The phone pipe with auto-reconnect off comes back once, as the ring does.
  {
    reset();
    enroll(BLE_PEER_G2_GLASSES, false);
    enroll(BLE_PEER_PHONE, false);
    glassesModeBoot();
    phoneUp();
    openconsole();
    CHECK(!sReconnect[BLE_PEER_PHONE].userDisconnect);
    serverMinutes();
    const unsigned admits = pipe::admits;
    openg2Saved();
    tick(20000);
    CHECK(pipe::admits == admits + 1 && pipe::last.explicitReseek && !pipe::last.autoReconnect);
    std::puts("auto-reconnect off, phone pipe up before openble: back once");
  }
  // A boot in server mode (openble persisted it) arms nothing; the first
  // `openg2 saved` brings the client host up and arms the ring and the phone.
  {
    reset();
    enroll(BLE_PEER_G2_GLASSES, false);
    enroll(BLE_PEER_R1_RING, true);
    enroll(BLE_PEER_PHONE, true);
    bleRegisterBuiltinPeers();
    gSettings.bleMode = BLE_MODE_SERVER;
    world.server = true;
    serverMinutes();
    const uint32_t back = gNow;
    openg2Saved();
    tick(20000);
    CHECK(jobs(BleConnectKind::RING_SAVED) == 1 && lastJob(BleConnectKind::RING_SAVED)->autoReconnect);
    CHECK(lastJob(BleConnectKind::RING_SAVED)->at - back <= 6000);
    CHECK(pipe::admits >= 1 && pipe::last.autoReconnect);
    std::puts("boot in server mode, then openg2 saved: ring and phone pipe come back");
  }
  // An openconsole refused before any teardown leaves the client stack and
  // its three links exactly as they were: the reseeks it requests find every
  // peer up, so nothing is scheduled.
  {
    reset();
    enroll(BLE_PEER_G2_GLASSES, true);
    enroll(BLE_PEER_R1_RING, true);
    enroll(BLE_PEER_PHONE, true);
    glassesModeBoot();
    glassesUp(); ringUp(); phoneUp();
    world.failServerStart = true;
    CHECK(std::strcmp(cmd_openconsole(String()),
                      "Error: Failed to start the app console server (the client stack is up; closed devices reconnect)") == 0);
    CHECK(world.client && !world.server && world.pipeActive && gPhoneReady);
    CHECK(gSettings.bleMode == BLE_MODE_G2_CLIENT);  // the saved role is back
    tick(20000);
    CHECK(jobs(BleConnectKind::RING_SAVED) == 0 && jobs(BleConnectKind::G2_SAVED) == 0);
    CHECK(pipe::admits == 0);  // the pipe stayed up
    std::puts("refused openconsole: nothing was taken down, nothing to bring back");
  }
  // An openconsole whose host start fails after the client side was closed
  // brings the peers it took down back through the reseeks it requested for
  // them (the client stack comes back for them; auto-reconnect is not needed).
  // The phone is left out here: its admission reopens a closed pipe only once
  // BLE_PhonePipe.cpp's open/close split lands (then it joins this check).
  {
    reset();
    enroll(BLE_PEER_G2_GLASSES, false);
    enroll(BLE_PEER_R1_RING, false);
    glassesModeBoot();
    glassesUp(); ringUp();
    world.failServerHost = true;
    CHECK(std::strcmp(cmd_openconsole(String()),
                      "Error: Failed to start the app console server (the client stack is up; closed devices reconnect)") == 0);
    CHECK(!world.server && world.client && gSettings.bleMode == BLE_MODE_G2_CLIENT);
    tick(20000);
    CHECK(jobs(BleConnectKind::RING_SAVED) == 1 && jobs(BleConnectKind::G2_SAVED) == 1);
    std::puts("failed openconsole host start: the glasses and ring it took down come back");
  }
  std::printf("BLE role switch reconnect: %ld checks passed\n", gChecks);
  return 0;
}
