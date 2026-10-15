// Production definitions are inserted by test_g2_client_lifecycle.py.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using String = std::string;
static std::atomic<unsigned> assertions{0};
static void check(bool ok, const char* message) {
  ++assertions;
  if (!ok) throw std::runtime_error(message);
}
// INSERT_TYPES
static std::mutex gControlLifecycleMux, gTopologyMux;
static thread_local std::vector<std::mutex*> held;
static void portENTER_CRITICAL(std::mutex* mux) {
  check(held.empty() || (held.size() == 1 && held.back() == &gControlLifecycleMux &&
        mux == &gTopologyMux), "lock order is lifecycle then topology");
  mux->lock(); held.push_back(mux);
}
static void portEXIT_CRITICAL(std::mutex* mux) {
  check(!held.empty() && held.back() == mux, "critical section releases its owner");
  held.pop_back(); mux->unlock();
}
static std::atomic<uint32_t> now{100};
static uint32_t millis() { check(held.empty(), "clock sampled outside critical section"); return now.load(); }
static void vTaskDelay(uint32_t ms) { check(held.empty(), "delay outside critical section"); now += ms; }
#define pdMS_TO_TICKS(ms) (ms)
#define DEBUG_G2F(...) do {} while (0)
#define BROADCAST_PRINTF(...) do {} while (0)
#define configASSERT(value) do { ++assertFailures; } while (0)
#define ENABLE_CAMERA_SENSOR 0
static unsigned assertFailures = 0;
static void broadcastOutput(const char*) { check(held.empty(), "logging outside critical section"); }
static uint8_t gControlStartBlocks = 0, gClientInitters = 0;
static uint8_t gUiWorkerInitters = 0, gUiSubmitters = 0;
#if TEST_RUNTIME
static G2ClientRuntime gClientRuntime;
#else
static G2ClientState* gG2State = nullptr;
enum class AllocPref { PreferPSRAM };
static void* ps_calloc(size_t n, size_t size, AllocPref, const char*) { return std::calloc(n, size); }
#endif

struct G2Temple {
  bool connected = false;
  uint32_t sessionReadyGeneration = 0, connectionGeneration = 0;
  uint16_t mtu = 0;
  uint32_t packetsSent = 0, packetsReceived = 0;
  char side = '?';
};
static G2Temple gL, gR;
static int8_t gBatteryL = -1, gBatteryR = -1;
using QueueHandle_t = void*;
using TaskHandle_t = void*;
static G2WorkerInitState gPageSwapInitState = G2WorkerInitState::Uninitialized;
static G2WorkerInitState gTapInitState = G2WorkerInitState::Uninitialized;
static QueueHandle_t gPageSwapQueue = nullptr, gTapQueue = nullptr;
static TaskHandle_t gPageSwapTaskH = nullptr, gTapTaskHandle = nullptr;
struct Scan {
  void setActiveScan(bool) {}
  void setInterval(unsigned) {}
  void setWindow(unsigned) {}
};
static Scan scan;
static Scan* gScan = nullptr;
struct Boundaries {
  bool role = true, memory = true, renderer = true, companion = false;
  bool stopped = true, server = false, serverStops = true, fault = false;
  bool ringBusy = false, ringQuiesces = true, halInit = true, halDeinit = true;
  bool leftQuiesced = true, rightQuiesced = true, scanPresent = true, scanReset = true;
  bool lensReady = true, tapReady = true, sessionReady = true;
  bool sessionStops = true, uiQuiesces = true, disconnect = true;
  bool connectIdle = true, scanIdle = true, connectStops = true;
  bool leftReset = true, rightReset = true;
  bool phoneStops = true;
  bool sampledMemory = false;
  size_t activeHeap = 0, stoppedHeap = 0;
  unsigned memorySamples = 0, serverStopCalls = 0, templeInits = 0;
  unsigned workerMask = 0, workerAllocations = 0, semaphoreAllocations = 0;
  unsigned sessionAllocations = 0, sessionRetirements = 0;
  std::vector<std::string> events;
  unsigned halStarts = 0, halStops = 0, peerRegisters = 0, templeResets = 0;
} boundary;
static bool phonePipeStop() { return boundary.phoneStops; }
static bool phonePipeHostStopped() { return boundary.stopped; }
static bool phonePipeStart(bool) { return true; }
static struct Settings { bool bleEnabled = true; int bleTxPower = 3; } gSettings;
enum class BleRoleTransition { IDLE, G2_START, G2_STOP, RECOVERING };
static BleRoleTransition roleState = BleRoleTransition::IDLE;
static bool bleRoleTransitionBegin(BleRoleTransition state) {
  if (!boundary.role) return false;
  check(roleState == BleRoleTransition::IDLE, "role claim starts idle");
  roleState = state; return true;
}
static void bleRoleTransitionEnd() { roleState = BleRoleTransition::IDLE; }
static BleRoleTransition bleRoleTransitionState() { return roleState; }
static bool bleRoleTransitionHeldByCurrentTask() { return roleState != BleRoleTransition::IDLE; }
static uint8_t gBleClientRetirementPeerMask = 0;
struct HalStatus { bool stopped() const { return boundary.stopped; } };
static HalStatus bluetoothHalStatus() { return {}; }
static void bleCentralClientsTerminalTeardownAcknowledged() { gBleClientRetirementPeerMask = 0; }
static bool bleStackLifecycleFaulted() { return boundary.fault; }
static void bleStackSetLifecycleFault(bool value) { boundary.fault = value; }
static bool radioCompanionBlocksRadio(const char**) { return boundary.companion; }
static bool g2RendererLivenessSelfTest() { return boundary.renderer; }
static size_t hw1InternalFreeBytes() {
  check(held.empty(), "memory sampling outside critical sections");
  check(roleState == BleRoleTransition::G2_START && gClientInitters == 1,
        "memory admission holds the existing role and initialization claims");
  ++boundary.memorySamples;
  boundary.events.emplace_back(boundary.stopped ? "memory-stopped" : "memory-active");
  if (!boundary.memory) return 0;
  if (boundary.sampledMemory) return boundary.stopped ? boundary.stoppedHeap : boundary.activeHeap;
  return gMemoryRequirements[0].minHeapBytes;
}
static struct ESPClass { size_t getFreePsram() const { return SIZE_MAX; } } ESP;
static void bleCentralTxInit() {}
static bool isBleServerInitialized() { return boundary.server; }
static void deinitBluetooth() {
  ++boundary.serverStopCalls; boundary.events.emplace_back("server-stop");
  if (boundary.serverStops) { boundary.server = false; boundary.stopped = true; }
}
static bool g2RingConnectInFlight() { return boundary.ringBusy; }
static bool g2RingPrepareForStackTeardown(unsigned) { return boundary.ringQuiesces; }
struct BluetoothHalShutdownResult { bool success; };
static BluetoothHalShutdownResult bluetoothHalDeinit() {
  check(held.empty(), "BLE teardown outside critical section");
  ++boundary.halStops; boundary.events.emplace_back("host-stop");
  if (boundary.halDeinit) boundary.stopped = true;
  return {boundary.halDeinit};
}
static bool bluetoothHalInit(const char*) {
  check(held.empty(), "BLE init outside critical section");
  ++boundary.halStarts; boundary.events.emplace_back("host-start");
  boundary.stopped = false; return boundary.halInit;
}
static bool bluetoothHalSupportsTxPower() { return false; }
using esp_err_t = int;
static constexpr int ESP_OK = 0;
static esp_err_t bluetoothHalSetTxPower(uint8_t) { return 0; }
static const char* esp_err_to_name(int) { return "stub"; }
static int constrain(int value, int low, int high) { return std::clamp(value, low, high); }
static constexpr unsigned MTU_TARGET = 247;
static void g2GapEventHandler() {}
struct BLEDevice {
  static void setCustomGapHandler(void (*)()) { check(held.empty(), "BLE callbacks changed outside lock"); }
  static void setMTU(unsigned) {}
  static Scan* getScan() { return boundary.scanPresent ? &scan : nullptr; }
};
static bool g2ScanCallbackResetAfterHostInit() { return boundary.scanReset; }
static bool templeRuntimeQuiescedForInit(G2Temple& temple) {
  boundary.events.emplace_back(&temple == &gL ? "left-quiesced" : "right-quiesced");
  return &temple == &gL ? boundary.leftQuiesced : boundary.rightQuiesced;
}
static void templeInit(G2Temple& temple, char side) {
  // Match the production reconstruction boundary: init owns a lifecycle
  // claim, rather than holding topology through allocation/destruction.
  check(held.empty(), "temple reconstruction remains outside critical sections");
  ++boundary.templeInits; boundary.events.emplace_back("temple-init");
  temple = {}; temple.side = side;
}
static bool templeReset(G2Temple& temple) {
  ++boundary.templeResets;
  return &temple == &gL ? boundary.leftReset : boundary.rightReset;
}
using SemaphoreHandle_t = void*;
static SemaphoreHandle_t gCreateAckSem = nullptr, gRebuildAckSem = nullptr, gImgPushAckSem = nullptr;
static SemaphoreHandle_t xSemaphoreCreateBinary() { ++boundary.semaphoreAllocations; return &scan; }
// These are allocation boundaries, not a second worker implementation. Page,
// tap and ring workers remain resident across runtime-only shutdown. Session
// shutdown retires that worker and its queue; the next admitted init recreates
// them. Model those distinct lifetimes while exercising real admission flow.
static bool workerBoundary(unsigned bit, bool ready) {
  boundary.events.emplace_back("worker-init");
  if (ready && !(boundary.workerMask & bit)) {
    boundary.workerMask |= bit; ++boundary.workerAllocations;
    if (bit == 8) ++boundary.sessionAllocations;
  }
  return ready;
}
static bool pageSwapInit() { return workerBoundary(1, boundary.lensReady); }
static bool tapDispatcherInit() { return workerBoundary(2, boundary.tapReady); }
static int g2PeerSpec = 0;
static void bleRegisterPeer(int) { ++boundary.peerRegisters; }
static void g2RingInit() { (void)workerBoundary(4, true); }
static void registerG2Pages() {}
static bool g2SessionEnsureWorker() { return workerBoundary(8, boundary.sessionReady); }
static bool g2SessionShutdown() {
  if (!boundary.sessionStops) return false;
  if (boundary.workerMask & 8) ++boundary.sessionRetirements;
  boundary.workerMask &= ~8U;
  return true;
}
static bool g2UiWorkersQuiesce(unsigned) { return boundary.uiQuiesces; }
static bool g2Disconnect() { return boundary.disconnect; }
static void g2RingDisconnect() {}
static bool bleConnectWorkerIdle() { return boundary.connectIdle; }
static bool g2ScanCallbacksIdle() { return boundary.scanIdle; }
static bool bleConnectShutdown() { return boundary.connectStops; }
static void stopHeartbeatTimer() {}
bool isG2ClientInitialized();
// INSERT_PRODUCTION

static void expectReleased() {
  check(roleState == BleRoleTransition::IDLE && gControlStartBlocks == 0 &&
        gClientInitters == 0 && held.empty(), "all owned init/teardown claims released");
}
static void defaults() {
  boundary = {}; gSettings = {}; now = 100; assertFailures = 0;
  gClientInitters = gControlStartBlocks = gUiWorkerInitters = gUiSubmitters = 0;
  roleState = BleRoleTransition::IDLE;
  gBleClientRetirementPeerMask = 0;
#if TEST_RUNTIME
  g2ClientRuntimeReset();
#else
  if (gG2State) { gG2State->~G2ClientState(); std::free(gG2State); gG2State = nullptr; }
#endif
  templeInit(gL, 'L'); templeInit(gR, 'R'); gScan = nullptr;
  gCreateAckSem = gRebuildAckSem = gImgPushAckSem = nullptr;
  boundary.templeInits = 0; boundary.events.clear();
}
static void expectOff() {
  check(!isG2ClientInitialized() && getG2State() == G2_STATE_IDLE &&
        std::strcmp(getG2StateString(), "off") == 0, "off retains public idle enum and off display");
#if TEST_RUNTIME
  check(g2ClientRuntimeSnapshot().connectedSince == 0, "off clears connection timestamp");
#endif
}
static void publishConnected() {
#if TEST_RUNTIME
  g2ClientConnected(true);
#else
  gG2State->state = G2_STATE_CONNECTED; gG2State->connectedSince = millis();
#endif
}
static void transitions() {
  defaults(); expectOff(); check(deinitG2Client(), "repeated stop while off succeeds"); expectReleased();
  const char* refusals[] = {"role", "fault", "companion", "claim", "renderer", "disabled", "memory",
                           "server", "ring-busy", "ring-quiesce", "normalize", "left-temple", "right-temple"};
  for (const char* reason : refusals) {
    defaults(); const std::string why(reason);
    if (why == "phone") boundary.phoneStops = false;
    if (why == "role") boundary.role = false;
    if (why == "fault") boundary.fault = true;
    if (why == "companion") boundary.companion = true;
    if (why == "claim") gControlStartBlocks = 1;
    if (why == "renderer") boundary.renderer = false;
    if (why == "disabled") gSettings.bleEnabled = false;
    if (why == "memory") boundary.memory = false;
    if (why == "server") { boundary.server = true; boundary.serverStops = false; }
    if (why == "ring-busy") { boundary.stopped = false; boundary.ringBusy = true; }
    if (why == "ring-quiesce") { boundary.stopped = false; boundary.ringQuiesces = false; }
    if (why == "normalize") { boundary.stopped = false; boundary.halDeinit = false; }
    if (why == "left-temple") boundary.leftQuiesced = false;
    if (why == "right-temple") boundary.rightQuiesced = false;
    check(!initG2Client(), "refused initialization fails"); expectOff();
    check(boundary.halStarts == 0, "refused startup never initializes host");
    if (why == "claim") { check(gControlStartBlocks == 1, "foreign blocker retained"); gControlStartBlocks = 0; }
    expectReleased(); std::cout << "init-refused:" << reason << '\n';
  }
  for (int failure = 0; failure < 3; ++failure) {
    for (bool rollback : {false, true}) {
      defaults(); boundary.halDeinit = rollback;
      if (failure == 0) boundary.halInit = false;
      if (failure == 1) boundary.scanPresent = false;
      if (failure == 2) boundary.scanReset = false;
      check(!initG2Client(), "host/scan failure rejects startup"); expectOff(); expectReleased();
      check(boundary.halStarts == 1 && boundary.halStops == 1 && boundary.fault == !rollback,
            "failed init checks rollback and marks failed teardown");
      std::cout << "rollback:" << failure << ':' << rollback << '\n';
    }
  }
  for (int workers = 0; workers < 8; ++workers) {
    defaults(); boundary.lensReady = (workers & 1) != 0; boundary.tapReady = (workers & 2) != 0;
    boundary.sessionReady = (workers & 4) != 0;
    check(initG2Client(), "worker allocation failure retains retryable initialized client");
    check(isG2ClientInitialized() && getG2State() == G2_STATE_IDLE &&
          std::strcmp(getG2StateString(), "idle") == 0, "successful init publishes ready idle");
    expectReleased(); publishConnected();
    check(initG2Client() && boundary.halStarts == 1 && boundary.peerRegisters == 1 &&
          getG2State() == G2_STATE_CONNECTED, "repeat init does not reset active runtime");
    expectReleased(); check(deinitG2Client(), "ready runtime stops"); expectOff(); expectReleased();
    check(gScan == nullptr, "successful stop clears scan handle");
    std::cout << "worker-init:" << workers << '\n';
  }
  const char* deferrals[] = {"role", "init", "ui-init", "ui-submit", "session", "ui", "disconnect",
                            "connect-idle", "scan", "connect-stop", "left-reset", "right-reset", "phone"};
  for (const char* reason : deferrals) {
    defaults(); check(initG2Client(), "prepare active teardown case"); publishConnected();
    const std::string why(reason);
    if (why == "phone") boundary.phoneStops = false;
    if (why == "role") boundary.role = false;
    if (why == "init") gClientInitters = 1;
    if (why == "ui-init") gUiWorkerInitters = 1;
    if (why == "ui-submit") gUiSubmitters = 1;
    if (why == "session") boundary.sessionStops = false;
    if (why == "ui") boundary.uiQuiesces = false;
    if (why == "disconnect") boundary.disconnect = false;
    if (why == "connect-idle") boundary.connectIdle = false;
    if (why == "scan") boundary.scanIdle = false;
    if (why == "connect-stop") boundary.connectStops = false;
    if (why == "left-reset") boundary.leftReset = false;
    if (why == "right-reset") boundary.rightReset = false;
    check(!deinitG2Client(), "busy lifetime owner defers shutdown");
    check(isG2ClientInitialized() && getG2State() == G2_STATE_CONNECTED,
          "deferred teardown retains readable initialized state");
#if TEST_RUNTIME
    check(g2ClientRuntimeSnapshot().connectedSince == 100, "deferred teardown retains connection timestamp");
#endif
    check(gControlStartBlocks == 0 && roleState == BleRoleTransition::IDLE,
          "deferred teardown releases only own claims");
    if (why == "init") check(gClientInitters == 1, "deinit does not consume foreign init claim");
    boundary = {}; gClientInitters = gUiWorkerInitters = gUiSubmitters = 0;
    check(deinitG2Client(), "deferred teardown remains retryable"); expectOff(); expectReleased();
    std::cout << "stop-deferred:" << reason << '\n';
  }
  defaults();
}

static size_t eventIndex(const char* event) {
  const auto found = std::find(boundary.events.begin(), boundary.events.end(), event);
  check(found != boundary.events.end(), "expected lifecycle boundary was reached");
  return static_cast<size_t>(found - boundary.events.begin());
}
static void expectNoNewClientResources() {
  check(boundary.templeInits == 0 && boundary.halStarts == 0 &&
        boundary.workerAllocations == 0 && boundary.semaphoreAllocations == 0,
        "refused admission does not reconstruct temples or allocate client/host workers");
}
static void expectAdmittedOrder() {
  check(eventIndex("memory-stopped") < eventIndex("temple-init") &&
        eventIndex("temple-init") < eventIndex("host-start") &&
        eventIndex("host-start") < eventIndex("worker-init"),
        "memory admission precedes every new temple, host and worker initialization");
#if TEST_POST_NORMALIZATION
  check(eventIndex("left-quiesced") < eventIndex("memory-stopped") &&
        eventIndex("right-quiesced") < eventIndex("memory-stopped"),
        "both old temples are quiesced before the post-cleanup memory check");
#endif
}
static void memoryAdmission() {
  const size_t minimum = gMemoryRequirements[0].minHeapBytes;
  check(minimum > 0 && gMemoryRequirements[0].minPsramBytes == 0,
        "G2 keeps its production internal-memory requirement");

  for (int offset : {-1, 0, 1}) {
    defaults(); boundary.sampledMemory = true;
    boundary.stoppedHeap = static_cast<size_t>(static_cast<int64_t>(minimum) + offset);
    check(initG2Client() == (offset >= 0), "cold startup obeys the exact production threshold");
    check(boundary.memorySamples == 1, "cold startup samples memory once");
    if (offset < 0) { expectOff(); expectNoNewClientResources(); }
    else { expectAdmittedOrder(); check(deinitG2Client(), "cold admitted runtime stops"); }
    expectReleased();
  }

  // A runtime stop retains the host and the page/tap/ring workers, but releases
  // session worker resources. Model heap returned by the next checked host
  // teardown, including a remaining shortage: reusing the persistent workers
  // must not silently weaken the registry floor.
  for (int offset : {-1, 0, 1}) {
    defaults(); check(initG2Client(), "prepare retained-worker restart");
    const unsigned workers = boundary.workerAllocations;
    check(workers == 4 && deinitG2Client() && !boundary.stopped &&
          boundary.workerMask == 7 && boundary.sessionRetirements == 1,
          "runtime stop retires session worker but retains page/tap/ring workers and old host");
    boundary.sampledMemory = true; boundary.activeHeap = minimum - 1;
    boundary.stoppedHeap = static_cast<size_t>(static_cast<int64_t>(minimum) + offset);
    boundary.halStarts = boundary.halStops = boundary.memorySamples = boundary.templeInits = 0;
    boundary.semaphoreAllocations = 0; boundary.events.clear();
    const bool admitted = TEST_POST_NORMALIZATION && offset >= 0;
    check(initG2Client() == admitted, "restart admission uses the intended host generation");
    check(boundary.memorySamples == 1 && boundary.halStops == TEST_POST_NORMALIZATION,
          "restart measures memory after checked host cleanup, or reproduces old early refusal");
    check(boundary.workerAllocations == workers + static_cast<unsigned>(admitted) &&
          boundary.sessionAllocations == 1 + static_cast<unsigned>(admitted) &&
          boundary.workerMask == (admitted ? 15U : 7U) && boundary.semaphoreAllocations == 0,
          "admitted restart recreates only session worker and reuses persistent workers and acknowledgements");
    if (admitted) {
      expectAdmittedOrder();
      check(eventIndex("host-stop") < eventIndex("memory-stopped"),
            "restart gate observes memory released by the old host");
      boundary.memory = false;
      const unsigned checks = boundary.memorySamples;
      check(initG2Client() && boundary.memorySamples == checks && boundary.halStarts == 1 &&
            boundary.workerAllocations == workers + 1 && boundary.sessionAllocations == 2,
            "already initialized client bypasses cleanup/admission without duplicate worker allocation");
      check(deinitG2Client(), "restarted client stops");
    } else {
      expectOff();
      check(boundary.stopped == static_cast<bool>(TEST_POST_NORMALIZATION),
            "post-cleanup shortage leaves the old host stopped for a later retry");
      check(boundary.templeInits == 0 && boundary.halStarts == 0,
            "refused retained-worker restart leaves no partial new host or temples");
#if TEST_POST_NORMALIZATION
      expectReleased(); boundary.stoppedHeap = minimum;
      check(!boundary.fault && initG2Client(),
            "raising available memory permits a same-instance retry after refused restart");
      check(boundary.memorySamples == 2 && boundary.halStops == 1 && boundary.halStarts == 1 &&
            boundary.workerAllocations == workers + 1 && boundary.sessionAllocations == 2 &&
            boundary.workerMask == 15 && boundary.semaphoreAllocations == 0,
            "retry recreates session once, reuses retained workers and keeps completed host cleanup");
      expectReleased(); check(deinitG2Client(), "retried retained-worker client stops");
#endif
    }
    expectReleased();
  }

  for (int offset : {-1, 0, 1}) {
    defaults(); boundary.server = true; boundary.stopped = false;
    boundary.sampledMemory = true; boundary.activeHeap = minimum - 1;
    boundary.stoppedHeap = static_cast<size_t>(static_cast<int64_t>(minimum) + offset);
    const bool admitted = TEST_POST_NORMALIZATION && offset >= 0;
    check(initG2Client() == admitted, "server-to-client admission uses post-server memory");
    check(boundary.serverStopCalls == TEST_POST_NORMALIZATION && boundary.memorySamples == 1,
          "server teardown precedes admission, or saved baseline refuses before teardown");
    if (admitted) {
      expectAdmittedOrder();
      check(eventIndex("server-stop") < eventIndex("memory-stopped"),
            "client gate follows successful server cleanup");
      check(deinitG2Client(), "server-to-client runtime stops");
    } else {
      expectOff(); expectNoNewClientResources();
      check(boundary.stopped == static_cast<bool>(TEST_POST_NORMALIZATION) &&
            boundary.server == !static_cast<bool>(TEST_POST_NORMALIZATION),
            "post-server shortage keeps the previous role stopped without starting a client");
#if TEST_POST_NORMALIZATION
      expectReleased(); boundary.stoppedHeap = minimum;
      check(!boundary.fault && initG2Client(),
            "raising available memory permits a same-instance retry after refused role switch");
      check(boundary.memorySamples == 2 && boundary.serverStopCalls == 1 && boundary.halStarts == 1 &&
            boundary.workerAllocations == 4,
            "role-switch retry preserves completed cleanup and allocates each absent worker once");
      expectReleased(); check(deinitG2Client(), "retried server-to-client runtime stops");
#endif
    }
    expectReleased();
  }

  // Simultaneous shortage and incomplete cleanup intentionally change refusal
  // precedence. A known lifecycle barrier must abort before the relocated gate.
  for (const char* reason : {"server", "ring-busy", "ring-quiesce", "normalize",
                             "left-temple", "right-temple"}) {
    defaults(); boundary.stopped = false; boundary.memory = false;
    const std::string why(reason);
    if (why == "server") { boundary.server = true; boundary.serverStops = false; }
    if (why == "ring-busy") boundary.ringBusy = true;
    if (why == "ring-quiesce") boundary.ringQuiesces = false;
    if (why == "normalize") boundary.halDeinit = false;
    if (why == "left-temple") boundary.leftQuiesced = false;
    if (why == "right-temple") boundary.rightQuiesced = false;
    check(!initG2Client(), "incomplete prior lifecycle refuses even with low memory");
    check(boundary.memorySamples == (TEST_POST_NORMALIZATION ? 0U : 1U),
          "cleanup barrier precedes relocated gate; saved baseline proves prior priority");
    check(boundary.fault == (TEST_POST_NORMALIZATION && why == "normalize"),
          "failed checked normalization preserves the lifecycle fault instead of hiding it");
    expectOff(); expectNoNewClientResources(); expectReleased();
  }
  defaults();
  std::cerr << "G2 memory admission (post-normalization=" << TEST_POST_NORMALIZATION
            << "): 15 boundary scenarios passed\n";
}

#if TEST_RUNTIME
static void workerAdmission() {
  defaults(); check(initG2Client(), "prepare worker admission");
  for (unsigned mask = 0; mask < 32; ++mask) {
    gPageSwapInitState = gTapInitState = mask & 1 ? G2WorkerInitState::Ready : G2WorkerInitState::Starting;
    gPageSwapQueue = gTapQueue = mask & 2 ? &scan : nullptr;
    gPageSwapTaskH = gTapTaskHandle = mask & 4 ? &gL : nullptr;
    gControlStartBlocks = mask & 8 ? 0 : 1;
    gUiSubmitters = mask & 16 ? 0 : UINT8_MAX;
    const bool expected = mask == 31;
    QueueHandle_t queue = &gR; TaskHandle_t task = &gR;
    check(pageSwapRuntimeSnapshot(&queue, &task) == expected, "page admission checks all prerequisites");
    check(queue == (expected ? &scan : nullptr) && task == (expected ? &gL : nullptr),
          "page admission publishes queue/task together or clears outputs");
    if (expected) { check(gUiSubmitters == 1, "page claim prevents cleanup until released"); g2UiSubmitDone(); }
    queue = &gR;
    check(tapRuntimeSnapshot(&queue) == expected && queue == (expected ? &scan : nullptr),
          "tap admission uses existing readiness gates");
    if (expected) { check(gUiSubmitters == 1, "tap claim prevents cleanup until released"); g2UiSubmitDone(); }
  }
  gControlStartBlocks = gUiSubmitters = 0;
  check(deinitG2Client(), "worker admission runtime stops");
  QueueHandle_t queue = &gR; TaskHandle_t task = &gR;
  check(!pageSwapRuntimeSnapshot(&queue, &task) && !queue && !task,
        "boot-lifetime page worker refuses uninitialized client");
  queue = &gR;
  check(!tapRuntimeSnapshot(&queue) && !queue && gUiSubmitters == 0,
        "boot-lifetime tap worker refuses uninitialized client without claim");
  defaults();
}

static void publicationAndConcurrency() {
  defaults();
  for (int state = G2_STATE_IDLE; state <= G2_STATE_ERROR; ++state) {
    g2ClientSetState(static_cast<G2State>(state)); g2ClientConnected(true); expectOff();
  }
  g2ClientRuntimeReset(true); now = 123;
  g2ClientConnected(true);
  check(g2ClientRuntimeSnapshot().connectedSince == 123, "new session stores clock sample");
  now = 456; g2ClientConnected(false);
  check(g2ClientRuntimeSnapshot().connectedSince == 123, "recovery retains session timestamp");
  g2ClientRuntimeReset(true); g2ClientConnected(false);
  check(g2ClientRuntimeSnapshot().connectedSince == 456, "recovery seeds absent timestamp");
  now = UINT32_MAX; g2ClientConnected(true);
  check(g2ClientRuntimeSnapshot().connectedSince == UINT32_MAX, "timestamp retains wrap boundary");
  const auto retained = g2ClientStatusSnapshot(); g2ClientRuntimeReset();
  check(retained.runtime.initialized && retained.runtime.state == G2_STATE_CONNECTED &&
        retained.runtime.connectedSince == UINT32_MAX, "runtime snapshots outlive owner resets");
  const unsigned before = assertions; (void)g2ClientStateName(retained);
  check(assertions == before, "state-name formatting does not resample or lock");
  std::atomic<bool> run{true};
  std::atomic<unsigned> reads{0};
  std::vector<std::thread> readers;
  for (unsigned i = 0; i < 4; ++i) readers.emplace_back([&] {
    while (run.load()) {
      const auto snapshot = g2ClientStatusSnapshot();
      if (!snapshot.runtime.initialized) {
        check(snapshot.runtime.state == G2_STATE_IDLE && snapshot.runtime.connectedSince == 0,
              "off publication is a complete record during concurrent reset");
      }
      if (snapshot.runtime.state == G2_STATE_CONNECTED)
        check(snapshot.runtime.initialized && snapshot.runtime.connectedSince != 0,
              "connected publication pairs timestamp and state");
      const char* name = g2ClientStateName(snapshot);
      const char* expected = !snapshot.runtime.initialized ? "off" :
          snapshot.runtime.state == G2_STATE_CONNECTED ?
          snapshot.links.left && snapshot.links.right ? "connected" :
          snapshot.links.left || snapshot.links.right ? "connecting" : "idle" :
          snapshot.runtime.state == G2_STATE_CONNECTING ? "connecting" : "idle";
      check(std::strcmp(name, expected) == 0, "state name uses exactly captured runtime and links");
      (void)isG2ClientInitialized(); (void)getG2State(); (void)getG2StateString();
      G2ConnectionStatus publicStatus;
      check(g2ConnectionStatusSnapshot(&publicStatus), "public status available during lifecycle traffic");
      if (std::strcmp(publicStatus.state, "connected") == 0)
        check(publicStatus.running && publicStatus.left.connected && publicStatus.right.connected,
              "public connected state uses the same captured topology");
      if (!publicStatus.running)
        check(std::strcmp(publicStatus.state, "off") == 0, "public running and off state agree");
      ++reads;
    }
  });
  now = 999;
  for (unsigned i = 0; i < 12000; ++i) {
    g2ClientRuntimeReset(true); g2ClientSetState(G2_STATE_CONNECTING);
    { std::lock_guard<std::mutex> lock(gTopologyMux); gL.connected = (i & 1) != 0; gR.connected = (i & 2) != 0; }
    g2ClientConnected(true); g2ClientConnected(false); g2ClientRuntimeReset();
    g2ClientSetState(G2_STATE_CONNECTED); g2ClientConnected(true);
  }
  // Repeated real lifecycle paths overlap status readers too. Driver resets
  // remain boundaries; this validates lifecycle publication, not BLE resources.
  for (unsigned i = 0; i < 100; ++i) {
    check(initG2Client(), "real initialization while readers poll");
    g2ClientConnected(true); check(deinitG2Client(), "real shutdown while readers poll");
  }
  run = false;
  for (auto& reader : readers) reader.join();
  check(reads > 0, "reader threads executed"); expectOff(); expectReleased();
  std::cerr << "Concurrent G2 snapshots: " << reads << '\n';
}
#endif

int main() {
  transitions();
  memoryAdmission();
#if TEST_RUNTIME
  workerAdmission();
  publicationAndConcurrency();
#endif
  std::cerr << "G2 lifecycle (runtime=" << TEST_RUNTIME << "): " << assertions << " assertions passed\n";
}
