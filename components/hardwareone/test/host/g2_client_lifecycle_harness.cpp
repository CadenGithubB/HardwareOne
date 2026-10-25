// Production definitions are inserted by test_g2_client_lifecycle.py.
//
// initG2Client / deinitG2Client are the glasses' device open/close on top of
// the shared BLE central stack (BLE_CentralStack.cpp). The stack owns the
// Bluedroid host, the controller link, the setup worker, the scan object, the
// GAP handler, the app console's command service on the client host (it
// registers in bleStackUp() on a fresh host, so this harness no longer has a
// console scenario) and peer registration plus auto-reconnect arming. The
// glasses reach it only through bleStackUp(), bleStackScan() and
// bleStackLastRefusal(). Nothing here fakes a host, pipe, ring or console
// call, so production that makes one again does not compile.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
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
static unsigned assertFailures = 0;
#define configASSERT(value) do { if (!(value)) ++assertFailures; } while (0)
#define ENABLE_CAMERA_SENSOR 0

// Worker bits. The ring worker is no longer part of the glasses lifecycle.
enum : unsigned { kPageWorker = 1, kTapWorker = 2, kSessionWorker = 8 };
constexpr unsigned kAllWorkers = kPageWorker | kTapWorker | kSessionWorker;

struct Boundaries {
  // Boundary results.
  bool role = true, renderer = true, stack = true, leftQuiesced = true, rightQuiesced = true;
  bool scanPresent = true, scanReset = true;
  bool lensReady = true, tapReady = true, sessionReady = true;
  bool sessionStops = true, uiQuiesces = true, disconnect = true, notesRelease = true;
  bool setupIdle = true, scanIdle = true, leftReset = true, rightReset = true, uiRelease = true;
  size_t heap = gMemoryRequirements[0].minHeapBytes;  // internal free bytes at admission
  const char* refusal = "";                          // bleStackLastRefusal()
  // A producer (a timer, the main loop, ESP-NOW, a ring gesture) runs once
  // when the lifecycle reaches the boundary named here; see reach().
  const char* raceAt = nullptr;
  // The shared host: started by the first admitted bleStackUp(). No glasses
  // path takes it down, which is why nothing here sets it back.
  bool stopped = true;
  // Observations.
  unsigned stackUps = 0, memorySamples = 0, templeInits = 0, templeResets = 0;
  unsigned workerMask = 0, workerAllocations = 0, semaphoreAllocations = 0;
  unsigned sessionRetirements = 0, peerRegisters = 0, repairEnds = 0;
  bool raced = false, racePublished = false;  // the producer ran; the runtime was published
  unsigned raceAllocations = 0;               // workers the producer got created
  int hijackPage = -1;
  std::vector<std::string> events, broadcasts;
} boundary;

static void broadcastOutput(const char* line) {
  check(held.empty(), "logging outside critical section");
  boundary.broadcasts.emplace_back(line ? line : "");
}
static void broadcastPrintf(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void broadcastPrintf(const char* format, ...) {
  char line[192];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  broadcastOutput(line);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)

static uint8_t gControlStartBlocks = 0, gClientInitters = 0;
static uint8_t gUiWorkerInitters = 0, gUiSubmitters = 0;
static G2ClientRuntime gClientRuntime;
bool isG2ClientInitialized();
static bool g2UiWorkerCreationOpenLocked();

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

// The lazy creators (fakes below). A producer calls them at any time; the
// creation gate decides whether any allocates.
static bool pageSwapInit();
static bool tapDispatcherInit();
static bool g2SessionEnsureWorker();
static unsigned producerCreates() {
  const unsigned before = boundary.workerAllocations;
  (void)pageSwapInit(); (void)tapDispatcherInit(); (void)g2SessionEnsureWorker();
  return boundary.workerAllocations - before;
}
// Every boundary records itself here. At boundary.raceAt a producer runs once,
// in the middle of the lifecycle step, and the harness keeps what it got.
static void reach(const char* event) {
  boundary.events.push_back(event);
  if (!boundary.raceAt || std::strcmp(boundary.raceAt, event) != 0) return;
  boundary.raceAt = nullptr;
  boundary.raced = true;
  boundary.racePublished = isG2ClientInitialized();
  boundary.raceAllocations = producerCreates();
}

enum class BleRoleTransition { IDLE, G2_START, G2_STOP };
static BleRoleTransition roleState = BleRoleTransition::IDLE;
static bool bleRoleTransitionBegin(BleRoleTransition state) {
  if (!boundary.role) return false;
  check(roleState == BleRoleTransition::IDLE, "role claim starts idle");
  roleState = state; return true;
}
static void bleRoleTransitionEnd() { roleState = BleRoleTransition::IDLE; }
static BleRoleTransition bleRoleTransitionState() { return roleState; }
static bool bleRoleTransitionHeldByCurrentTask() { return roleState != BleRoleTransition::IDLE; }

// The shared central stack. bleStackUp() brings the host up or reuses it; its
// refusals (Bluetooth disabled, a faulted teardown, the radio companion, the
// console server, a busy or unquiesced ring, a failed normalization, its own
// "blestack" memory row) are one boundary result with a reason.
struct BLEScan {};
static BLEScan scan;
static BLEScan* gScan = nullptr;
static bool bleStackUp() {
  check(held.empty(), "the stack comes up outside critical sections");
  check(roleState == BleRoleTransition::G2_START && gClientInitters == 1,
        "the glasses bring the stack up inside their role and initialization claims");
  check(!isG2ClientInitialized(), "the stack comes up before the runtime is published");
  ++boundary.stackUps; reach("stack-up");
  if (!boundary.stack) return false;
  boundary.stopped = false; return true;
}
static BLEScan* bleStackScan() {
  check(!boundary.stopped, "the scan object is read from the running stack");
  reach("stack-scan");
  return boundary.scanPresent ? &scan : nullptr;
}
static const char* bleStackLastRefusal() { return boundary.refusal; }

static bool g2RendererLivenessSelfTest() { return boundary.renderer; }
static bool templeRuntimeQuiescedForInit(G2Temple& temple) {
  reach(&temple == &gL ? "left-quiesced" : "right-quiesced");
  return &temple == &gL ? boundary.leftQuiesced : boundary.rightQuiesced;
}
static void templeInit(G2Temple& temple, char side) {
  // Match the production reconstruction boundary: init owns a lifecycle
  // claim, rather than holding topology through allocation/destruction.
  check(held.empty(), "temple reconstruction remains outside critical sections");
  ++boundary.templeInits; reach("temple-init");
  temple = {}; temple.side = side;
}
static bool templeReset(G2Temple& temple) {
  ++boundary.templeResets; reach(&temple == &gL ? "left-reset" : "right-reset");
  return &temple == &gL ? boundary.leftReset : boundary.rightReset;
}
// The runtime's own budget is sampled once the shared stack is up: the
// "g2client" row counts the glasses runtime only ("blestack" is the stack's).
static size_t hw1InternalFreeBytes() {
  check(held.empty(), "memory sampling outside critical sections");
  check(roleState == BleRoleTransition::G2_START && gClientInitters == 1,
        "memory admission holds the existing role and initialization claims");
  check(!boundary.stopped, "the runtime budget is sampled with the shared stack up");
  ++boundary.memorySamples; reach("memory");
  return boundary.heap;
}
static struct ESPClass { size_t getFreePsram() const { return SIZE_MAX; } } ESP;
static bool g2ScanCallbackResetAfterHostInit() {
  reach("scan-reset"); return boundary.scanReset;
}
using SemaphoreHandle_t = void*;
static SemaphoreHandle_t gCreateAckSem = nullptr, gRebuildAckSem = nullptr, gImgPushAckSem = nullptr;
static SemaphoreHandle_t xSemaphoreCreateBinary() {
  ++boundary.semaphoreAllocations; reach("ack-semaphore"); return &scan;
}

// Allocation boundaries, not a second worker implementation. Each creator
// consults the production lazy-creation gate (g2UiWorkerCreationOpenLocked)
// under the lifecycle lock before it would claim its slot; the source check
// in the .py holds pageSwapInit, tapDispatcherInit and g2SessionEnsureWorker
// to the same. g2UiWorkersRelease releases page and tap (keeps them when it
// times out); g2SessionShutdown retires the session worker.
static bool workerBoundary(unsigned bit, bool ready, const char* event) {
  reach(event);
  if (boundary.workerMask & bit) return true;
  portENTER_CRITICAL(&gControlLifecycleMux);
  const bool open = g2UiWorkerCreationOpenLocked();
  portEXIT_CRITICAL(&gControlLifecycleMux);
  if (!open || !ready) return false;
  boundary.workerMask |= bit; ++boundary.workerAllocations;
  return true;
}
static bool pageSwapInit() { return workerBoundary(kPageWorker, boundary.lensReady, "page-init"); }
static bool tapDispatcherInit() { return workerBoundary(kTapWorker, boundary.tapReady, "tap-init"); }
static bool g2SessionEnsureWorker() {
  return workerBoundary(kSessionWorker, boundary.sessionReady, "session-init");
}

static bool g2SessionShutdown() {
  check(gControlStartBlocks != 0, "the session worker retires under the start block");
  reach("session-shutdown");
  if (!boundary.sessionStops) return false;
  if (boundary.workerMask & kSessionWorker) ++boundary.sessionRetirements;
  boundary.workerMask &= ~kSessionWorker;
  return true;
}
static bool g2UiWorkersQuiesce(uint32_t timeoutMs) {
  check(timeoutMs > 0, "the UI quiesce is bounded");
  reach("ui-quiesce"); return boundary.uiQuiesces;
}
static bool g2Disconnect(bool = false) {
  reach("disconnect"); return boundary.disconnect;
}
static bool g2NativeNotesRelease(uint32_t timeoutMs) {
  check(timeoutMs > 0, "the notes worker release is bounded");
  reach("notes-release"); return boundary.notesRelease;
}
// The setup worker is shared: the ring and the phone keep their own slots
// and links. The glasses wait for their own slot only.
namespace hw1setup {
enum class Client : uint8_t { G2 = 0, RingRetire = 1, Ring = 2, Phone = 3 };
constexpr uint8_t clientMask(Client c) { return uint8_t(1u << unsigned(c)); }
}
static bool bleSetupWaitIdle(uint8_t mask, uint32_t timeoutMs) {
  check(mask == hw1setup::clientMask(hw1setup::Client::G2) && timeoutMs > 0,
        "deinit waits only for the glasses' own setup slot, bounded");
  reach("setup-idle"); return boundary.setupIdle;
}
static void endRepairEpisode() { ++boundary.repairEnds; reach("repair-end"); }
static bool g2ScanCallbacksIdle() { reach("scan-idle"); return boundary.scanIdle; }
static void stopHeartbeatTimer() { reach("heartbeat-stop"); }
static void g2NotifyClearTimerStop() { reach("notify-clear-stop"); }
static void g2SetHijackPage(G2HijackPage page) {
  boundary.hijackPage = page;
  reach(page == G2_HIJACK_PAGE_MAIN ? "hijack-main" : "hijack-other");
}
static bool g2UiWorkersRelease(uint32_t timeoutMs) {
  // The real release acts only in this state (G2_Glasses.cpp g2UiWorkersRelease)
  // and otherwise returns false having released nothing.
  portENTER_CRITICAL(&gControlLifecycleMux);
  const bool open = gControlStartBlocks != 0 && !gClientRuntime.initialized &&
      gUiSubmitters == 0 && gUiWorkerInitters == 0;
  portEXIT_CRITICAL(&gControlLifecycleMux);
  check(open && timeoutMs > 0, "the UI workers are released after the runtime is retired, "
        "under the start block, with no producer left, bounded");
  reach("ui-release");
  if (!boundary.uiRelease) return false;
  boundary.workerMask &= ~(kPageWorker | kTapWorker);
  return true;
}
static void g2RegisterBlePeer() {
  check(isG2ClientInitialized(), "the glasses' peer registers on the published runtime");
  ++boundary.peerRegisters; reach("peer-register");
}
static void registerG2Pages() { reach("pages-register"); }
static const char* g2RepairDescribe(bool, bool, char (&)[8]) { return "none"; }
// INSERT_PRODUCTION

// A successful stop reaches these boundaries, in this order. A deferred stop
// reaches a prefix of them, ending at the barrier that deferred it.
static const char* const kTeardown[] = {
    "session-shutdown", "ui-quiesce", "disconnect", "notes-release", "setup-idle",
    "repair-end", "scan-idle", "heartbeat-stop", "left-reset", "right-reset",
    "notify-clear-stop", "hijack-main", "ui-release"};
static constexpr size_t kTeardownSteps = sizeof(kTeardown) / sizeof(kTeardown[0]);
static size_t teardownStep(const char* event) {
  for (size_t i = 0; i < kTeardownSteps; ++i) if (std::strcmp(kTeardown[i], event) == 0) return i;
  check(false, "known teardown boundary");
  return kTeardownSteps;
}
static bool teardownReached(size_t steps) {
  if (boundary.events.size() != steps) return false;
  for (size_t i = 0; i < steps; ++i) if (boundary.events[i] != kTeardown[i]) return false;
  return true;
}

static void expectReleased() {
  check(roleState == BleRoleTransition::IDLE && gControlStartBlocks == 0 &&
        gClientInitters == 0 && held.empty(), "all owned init/teardown claims released");
}
static void defaults() {
  boundary = {}; now = 100; assertFailures = 0;
  gClientInitters = gControlStartBlocks = gUiWorkerInitters = gUiSubmitters = 0;
  roleState = BleRoleTransition::IDLE;
  g2ClientRuntimeReset();
  templeInit(gL, 'L'); templeInit(gR, 'R'); gScan = nullptr;
  gCreateAckSem = gRebuildAckSem = gImgPushAckSem = nullptr;
  gPageSwapInitState = gTapInitState = G2WorkerInitState::Uninitialized;
  gPageSwapQueue = gTapQueue = nullptr; gPageSwapTaskH = gTapTaskHandle = nullptr;
  boundary.templeInits = 0; boundary.events.clear();
}
// Counters for a restart on the same instance; the stack and workers stay.
static void restartCounters() {
  boundary.stackUps = boundary.memorySamples = boundary.templeInits = 0;
  boundary.workerAllocations = boundary.semaphoreAllocations = boundary.peerRegisters = 0;
  boundary.sessionRetirements = 0;
  boundary.events.clear(); boundary.broadcasts.clear();
}
static void expectOff() {
  check(!isG2ClientInitialized() && getG2State() == G2_STATE_IDLE &&
        std::strcmp(getG2StateString(), "off") == 0, "off retains public idle enum and off display");
  check(g2ClientRuntimeSnapshot().connectedSince == 0, "off clears connection timestamp");
}
static void publishConnected() { g2ClientConnected(true); }
static size_t eventAt(const char* event) {
  const auto found = std::find(boundary.events.begin(), boundary.events.end(), event);
  return found == boundary.events.end() ? SIZE_MAX : static_cast<size_t>(found - boundary.events.begin());
}
static size_t eventIndex(const char* event) {
  const size_t at = eventAt(event);
  check(at != SIZE_MAX, "expected lifecycle boundary was reached");
  return at;
}
static bool broadcast(const std::string& line) {
  return std::find(boundary.broadcasts.begin(), boundary.broadcasts.end(), line) !=
         boundary.broadcasts.end();
}
static void expectNoNewClientResources() {
  check(boundary.templeInits == 0 && boundary.workerAllocations == 0 &&
        boundary.semaphoreAllocations == 0 && boundary.peerRegisters == 0,
        "refused admission reconstructs no temple and allocates no acknowledgement, worker or peer");
}
// initG2Client opens the runtime on the running stack: the stack is brought up
// (or reused) first, the temple barrier and the runtime's own memory gate
// follow, every temple, acknowledgement and worker allocation comes after the
// gate, and the session worker is the last step.
static void expectAdmittedOrder() {
  const size_t stack = eventIndex("stack-up"), memory = eventIndex("memory");
  check(stack < eventIndex("left-quiesced") && stack < eventIndex("right-quiesced"),
        "the shared stack is up before the temple barrier");
  check(eventIndex("left-quiesced") < memory && eventIndex("right-quiesced") < memory,
        "both old temples are quiesced before the runtime memory gate");
  for (const char* allocation : {"temple-init", "page-init", "tap-init", "session-init"})
    check(memory < eventIndex(allocation), "memory admission precedes every temple and worker");
  check(eventAt("ack-semaphore") == SIZE_MAX || memory < eventIndex("ack-semaphore"),
        "memory admission precedes the acknowledgement allocations");
  check(boundary.events.back() == "session-init", "the session worker is the last step of a start");
}

static void transitions() {
  defaults(); expectOff(); check(deinitG2Client(), "repeated stop while off succeeds"); expectReleased();
  check(boundary.events.empty(), "a stop while off reaches no teardown boundary");

  // Refusals ahead of the stack start nothing; later ones leave the shared
  // stack up (the ring and the phone may use it). None reconstructs a temple
  // or allocates an acknowledgement, a worker or a peer.
  const char* refusals[] = {"role", "claim", "init", "renderer", "stack", "memory",
                            "left-temple", "right-temple"};
  for (const char* reason : refusals) {
    defaults(); const std::string why(reason);
    if (why == "role") boundary.role = false;
    if (why == "claim") gControlStartBlocks = 1;
    if (why == "init") gClientInitters = 1;
    if (why == "renderer") boundary.renderer = false;
    if (why == "stack") boundary.stack = false;
    if (why == "memory") boundary.heap = 0;
    if (why == "left-temple") boundary.leftQuiesced = false;
    if (why == "right-temple") boundary.rightQuiesced = false;
    check(!initG2Client(), "refused initialization fails"); expectOff(); expectNoNewClientResources();
    const bool beforeStack = why == "role" || why == "claim" || why == "init" || why == "renderer";
    check(boundary.stackUps == (beforeStack ? 0U : 1U) &&
          boundary.stopped == (beforeStack || why == "stack"),
          "a refusal never tears the shared stack down; one ahead of it never starts it");
    check(assertFailures == ((why == "left-temple" || why == "right-temple") ? 1U : 0U),
          "an unquiesced temple trips the production assertion");
    if (why == "claim") { check(gControlStartBlocks == 1, "foreign blocker retained"); gControlStartBlocks = 0; }
    if (why == "init") { check(gClientInitters == 1, "foreign init claim retained"); gClientInitters = 0; }
    expectReleased(); std::cout << "init-refused:" << reason << '\n';
  }

  // The refusals the stack owns now reach the user with the stack's reason.
  const char* stackReasons[] = {
      "Bluetooth is disabled - run 'bleenabled 1' first",
      "a prior Bluetooth teardown was incomplete; reboot required",
      "the app console server owns the Bluetooth host; run 'closeconsole' first",
      "a ring connection is still active; retry shortly",
      "not enough internal DRAM for the Bluetooth host"};
  for (const char* reason : stackReasons) {
    defaults(); boundary.stack = false; boundary.refusal = reason;
    check(!initG2Client(), "a stack refusal fails the start"); expectOff(); expectReleased();
    check(broadcast(std::string("[G2] Client start blocked: ") + reason),
          "the stack's refusal reason is broadcast");
    check(boundary.events.size() == 1 && boundary.memorySamples == 0,
          "a stack refusal precedes the temple barrier and the runtime memory gate");
  }

  // A scan failure resets the runtime and makes no host call: the stack is
  // shared, and no rollback fake exists here. The same instance retries.
  for (bool present : {false, true}) {
    defaults(); boundary.scanPresent = present; boundary.scanReset = false;
    check(!initG2Client(), "scan failure rejects startup"); expectOff(); expectReleased();
    check(gScan == nullptr && broadcast("[G2] Scan init failed"), "a scan failure drops the cached scan");
    check(boundary.stackUps == 1 && !boundary.stopped, "a scan failure leaves the shared stack up");
    check(boundary.workerAllocations == 0 && boundary.semaphoreAllocations == 0 &&
          boundary.peerRegisters == 0, "a scan failure precedes every worker, acknowledgement and peer");
    boundary.scanPresent = boundary.scanReset = true;
    check(initG2Client() && boundary.stackUps == 2 && boundary.workerMask == kAllWorkers,
          "a failed start retries on the running stack");
    check(deinitG2Client(), "the retried start stops"); expectOff(); expectReleased();
    std::cout << "scan-failed:" << present << '\n';
  }

  for (int workers = 0; workers < 8; ++workers) {
    defaults(); boundary.lensReady = (workers & 1) != 0; boundary.tapReady = (workers & 2) != 0;
    boundary.sessionReady = (workers & 4) != 0;
    const unsigned created = ((workers & 1) ? kPageWorker : 0U) | ((workers & 2) ? kTapWorker : 0U) |
                             ((workers & 4) ? kSessionWorker : 0U);
    check(initG2Client(), "worker allocation failure retains retryable initialized client");
    check(isG2ClientInitialized() && getG2State() == G2_STATE_IDLE &&
          std::strcmp(getG2StateString(), "idle") == 0, "successful init publishes ready idle");
    check(boundary.workerMask == created &&
          boundary.workerAllocations == static_cast<unsigned>(__builtin_popcount(created)),
          "admission opens the gate for each eager worker that can allocate, once");
    check(broadcast("[G2] UI worker memory unavailable; lens actions may be rejected") == ((workers & 3) != 3),
          "a missing UI worker is reported and is not fatal");
    check(boundary.semaphoreAllocations == 3 && boundary.peerRegisters == 1,
          "a start allocates the acknowledgements and registers the glasses' peer once");
    expectAdmittedOrder(); expectReleased(); publishConnected();
    const unsigned samples = boundary.memorySamples, allocations = boundary.workerAllocations;
    check(initG2Client() && boundary.stackUps == 1 && boundary.memorySamples == samples &&
          boundary.peerRegisters == 1 && boundary.workerAllocations == allocations &&
          getG2State() == G2_STATE_CONNECTED,
          "repeat init returns before the stack, the gate and every allocation, keeping the runtime");
    expectReleased(); boundary.events.clear();
    check(deinitG2Client(), "ready runtime stops"); expectOff(); expectReleased();
    check(teardownReached(kTeardownSteps), "a stop closes the runtime in the production order");
    check(boundary.repairEnds == 1 && gScan == nullptr && boundary.hijackPage == G2_HIJACK_PAGE_MAIN,
          "a stop ends the one-temple repair, drops the scan and returns the lens to the main page");
    check(boundary.workerMask == 0 && boundary.sessionRetirements == ((workers & 4) ? 1U : 0U),
          "a stop retires the session worker and releases both UI workers");
    check(!boundary.stopped && boundary.stackUps == 1, "a stop never touches the shared stack");
    std::cout << "worker-init:" << workers << '\n';
  }

  const char* deferrals[] = {"role", "init", "ui-init", "ui-submit", "session", "ui", "disconnect",
                             "notes", "setup-idle", "scan", "left-reset", "right-reset"};
  for (const char* reason : deferrals) {
    defaults(); check(initG2Client(), "prepare active teardown case"); publishConnected();
    boundary.events.clear();
    const std::string why(reason);
    bool* barrier = nullptr;    // the boundary result that defers this stop
    const char* last = nullptr; // the last teardown boundary it reaches
    if (why == "role") barrier = &boundary.role;
    if (why == "init") gClientInitters = 1;
    if (why == "ui-init") gUiWorkerInitters = 1;
    if (why == "ui-submit") gUiSubmitters = 1;
    if (why == "session") { barrier = &boundary.sessionStops; last = "session-shutdown"; }
    if (why == "ui") { barrier = &boundary.uiQuiesces; last = "ui-quiesce"; }
    if (why == "disconnect") { barrier = &boundary.disconnect; last = "disconnect"; }
    if (why == "notes") { barrier = &boundary.notesRelease; last = "notes-release"; }
    if (why == "setup-idle") { barrier = &boundary.setupIdle; last = "setup-idle"; }
    if (why == "scan") { barrier = &boundary.scanIdle; last = "scan-idle"; }
    // Both temples are reset before either result is checked.
    if (why == "left-reset") { barrier = &boundary.leftReset; last = "right-reset"; }
    if (why == "right-reset") { barrier = &boundary.rightReset; last = "right-reset"; }
    if (barrier) *barrier = false;
    check(!deinitG2Client(), "busy lifetime owner defers shutdown");
    check(isG2ClientInitialized() && getG2State() == G2_STATE_CONNECTED,
          "deferred teardown retains readable initialized state");
    check(g2ClientRuntimeSnapshot().connectedSince == 100, "deferred teardown retains connection timestamp");
    const size_t reached = last ? teardownStep(last) + 1 : 0;
    check(teardownReached(reached), "a deferred stop ends at its barrier and runs nothing after it");
    check(boundary.workerMask == (kPageWorker | kTapWorker | (reached <= 1 ? kSessionWorker : 0U)),
          "a deferred stop keeps both UI workers; only a completed session shutdown retires its worker");
    check(gControlStartBlocks == 0 && roleState == BleRoleTransition::IDLE,
          "deferred teardown releases only own claims");
    if (why == "init") check(gClientInitters == 1, "deinit does not consume foreign init claim");
    else expectReleased();
    if (barrier) *barrier = true;
    gClientInitters = gUiWorkerInitters = gUiSubmitters = 0;
    boundary.events.clear();
    check(deinitG2Client(), "deferred teardown remains retryable"); expectOff(); expectReleased();
    check(teardownReached(kTeardownSteps) && boundary.workerMask == 0,
          "the retried stop runs the whole sequence and releases every worker");
    std::cout << "stop-deferred:" << reason << '\n';
  }
  defaults();
}

static void memoryAdmission() {
  const size_t minimum = gMemoryRequirements[0].minHeapBytes;
  check(minimum > 0 && gMemoryRequirements[0].minPsramBytes == 0,
        "G2 keeps its production internal-memory requirement");
  const auto heapAt = [minimum](int offset) {
    return static_cast<size_t>(static_cast<int64_t>(minimum) + offset);
  };

  // Cold start: the stack comes up first (its own "blestack" row is its
  // business), then the runtime's "g2client" row is checked once. A refusal
  // reconstructs no temple, allocates nothing and leaves the stack up.
  for (int offset : {-1, 0, 1}) {
    defaults(); boundary.heap = heapAt(offset);
    check(initG2Client() == (offset >= 0), "cold startup obeys the exact production threshold");
    check(boundary.memorySamples == 1 && boundary.stackUps == 1 && !boundary.stopped,
          "cold startup samples memory once, on the running stack");
    if (offset < 0) {
      expectOff(); expectNoNewClientResources();
      check(std::any_of(boundary.broadcasts.begin(), boundary.broadcasts.end(), [](const std::string& line) {
              return line.rfind("[G2] Client start refused: Insufficient heap: need ", 0) == 0;
            }), "the production checker's reason reaches the user");
    } else {
      expectAdmittedOrder(); check(deinitG2Client(), "cold admitted runtime stops");
    }
    expectReleased();
  }

  // Restart after a complete stop: page, tap and session were released and
  // the stack stayed up. The floor applies unchanged; an admitted restart
  // recreates the three workers once and reuses the acknowledgements; a
  // refused one leaves nothing behind, and the same instance retries.
  for (int offset : {-1, 0, 1}) {
    defaults(); check(initG2Client(), "prepare restart");
    check(boundary.workerAllocations == 3 && deinitG2Client() && boundary.workerMask == 0 &&
          boundary.sessionRetirements == 1 && !boundary.stopped,
          "a stop releases page, tap and session and leaves the shared stack up");
    restartCounters(); boundary.heap = heapAt(offset);
    const bool admitted = offset >= 0;
    check(initG2Client() == admitted, "restart obeys the exact production threshold");
    check(boundary.stackUps == 1 && boundary.memorySamples == 1,
          "restart reuses the running stack and samples memory once");
    check(boundary.workerAllocations == (admitted ? 3U : 0U) && boundary.semaphoreAllocations == 0,
          "an admitted restart recreates page, tap and session once and reuses the acknowledgements");
    if (admitted) {
      expectAdmittedOrder();
      boundary.heap = 0;
      check(initG2Client() && boundary.memorySamples == 1 && boundary.stackUps == 1 &&
            boundary.workerAllocations == 3,
            "an initialized client bypasses the stack and admission without duplicate allocation");
      check(deinitG2Client(), "restarted client stops");
    } else {
      expectOff(); expectNoNewClientResources(); expectReleased();
      check(boundary.workerMask == 0 && !boundary.stopped,
            "a refused restart leaves no worker behind and the shared stack up");
      boundary.heap = minimum;
      check(initG2Client() && boundary.memorySamples == 2 && boundary.workerAllocations == 3 &&
            boundary.workerMask == kAllWorkers && boundary.semaphoreAllocations == 0,
            "raising available memory permits a same-instance retry that creates each worker once");
      expectReleased(); check(deinitG2Client(), "retried client stops");
    }
    expectReleased();
  }

  // A UI release that timed out kept page and tap (fail closed); the stop
  // still completed. Reusing them does not weaken the floor: the restart is
  // admitted or refused exactly as above and recreates only the session worker.
  for (int offset : {-1, 0, 1}) {
    defaults(); check(initG2Client(), "prepare kept-worker restart");
    boundary.uiRelease = false; boundary.events.clear();
    check(deinitG2Client(), "a UI release timeout never fails the stop"); expectOff(); expectReleased();
    check(teardownReached(kTeardownSteps) && boundary.workerMask == (kPageWorker | kTapWorker) &&
          boundary.sessionRetirements == 1,
          "a timed-out release keeps page and tap; the session worker is retired");
    boundary.uiRelease = true; restartCounters(); boundary.heap = heapAt(offset);
    const bool admitted = offset >= 0;
    check(initG2Client() == admitted && boundary.memorySamples == 1,
          "kept workers do not weaken the production threshold");
    check(boundary.workerAllocations == (admitted ? 1U : 0U) &&
          boundary.workerMask == (admitted ? kAllWorkers : kPageWorker | kTapWorker),
          "a restart recreates only the session worker and reuses the kept UI workers");
    if (!admitted) {
      expectOff(); expectNoNewClientResources(); expectReleased();
      boundary.heap = minimum;
      check(initG2Client() && boundary.workerAllocations == 1 && boundary.workerMask == kAllWorkers,
            "the same-instance retry reuses the kept workers");
    }
    expectReleased();
    check(deinitG2Client() && boundary.workerMask == 0, "a later stop releases the kept workers");
    expectReleased();
  }

  // Low memory together with a refusal ahead of the runtime gate: the stack's
  // refusal and an unquiesced temple refuse first, without sampling memory.
  for (const char* reason : {"stack", "left-temple", "right-temple"}) {
    defaults(); boundary.heap = 0;
    const std::string why(reason);
    if (why == "stack") boundary.stack = false;
    if (why == "left-temple") boundary.leftQuiesced = false;
    if (why == "right-temple") boundary.rightQuiesced = false;
    check(!initG2Client(), "an earlier refusal fails the start even with low memory");
    check(boundary.memorySamples == 0, "the stack and temple barriers precede the runtime memory gate");
    expectOff(); expectNoNewClientResources(); expectReleased();
  }
  defaults();
  std::cerr << "G2 memory admission: 12 boundary scenarios passed\n";
}

// The lazy-creation gate (production g2UiWorkerCreationOpenLocked) admits a UI
// worker only for a published runtime or an admitted one: initG2Client opens
// it after its last refusal, just before the eager creation, and the runtime
// reset closes it. An init claim alone does not open it, so a producer racing
// a start that the stack, the temple barrier, the memory floor or the scan
// then refuse leaves no 8-10 KB task behind. Neither does one racing a stop
// that holds the start block, nor one calling while the glasses are closed.
static void workerCreationGate() {
  struct Race { const char* at; const char* refusal; };
  const Race refusedStarts[] = {{"stack-up", "stack"}, {"left-quiesced", "temple"},
                                {"memory", "memory"}, {"stack-scan", "scan"},
                                {"scan-reset", "scan-reset"}};
  for (const Race& race : refusedStarts) {
    defaults(); boundary.raceAt = race.at;
    const std::string why(race.refusal);
    if (why == "stack") boundary.stack = false;
    if (why == "temple") boundary.leftQuiesced = false;
    if (why == "memory") boundary.heap = 0;
    if (why == "scan") boundary.scanPresent = false;
    if (why == "scan-reset") boundary.scanReset = false;
    check(!initG2Client() && boundary.raced && !boundary.racePublished,
          "the producer raced a start that was then refused");
    check(boundary.raceAllocations == 0 && boundary.workerMask == 0,
          "a producer racing a start that is then refused creates no UI worker");
    check(producerCreates() == 0, "a refused start leaves the creation gate closed");
    expectOff(); expectReleased();
    std::cout << "race-refused-start:" << race.at << '\n';
  }
  // The gate opens between the last refusal and the eager creation.
  defaults(); boundary.raceAt = "scan-reset";
  check(initG2Client() && boundary.raced && boundary.raceAllocations == 0,
        "until the last refusal has passed, a producer creates nothing");
  check(boundary.workerMask == kAllWorkers && boundary.workerAllocations == 3,
        "admission opens the gate for the eager creation");
  check(deinitG2Client(), "the raced start stops"); expectOff(); expectReleased();
  defaults(); boundary.raceAt = "page-init";
  check(initG2Client() && boundary.raced && !boundary.racePublished && boundary.raceAllocations == 3,
        "from admission, a producer that beats the publication creates the workers");
  check(boundary.workerMask == kAllWorkers && boundary.workerAllocations == 3,
        "the start reuses what the producer created: each worker once");
  check(deinitG2Client() && boundary.workerMask == 0, "the stop releases the producer's workers");
  expectOff(); expectReleased();

  // Closed glasses and a stop holding the start block create nothing; a
  // published runtime retries each missing worker.
  defaults();
  check(producerCreates() == 0, "closed glasses create no UI worker");
  boundary.lensReady = boundary.tapReady = boundary.sessionReady = false;
  check(initG2Client() && boundary.workerMask == 0, "a start whose workers cannot allocate still opens");
  boundary.lensReady = boundary.tapReady = boundary.sessionReady = true;
  boundary.raceAt = "notify-clear-stop";
  check(deinitG2Client() && boundary.raced && boundary.racePublished && boundary.raceAllocations == 0,
        "a producer racing the stop creates no UI worker, though the runtime is still published");
  check(producerCreates() == 0, "a stopped client creates no UI worker");
  expectOff(); expectReleased();
  boundary.lensReady = boundary.tapReady = boundary.sessionReady = false;
  check(initG2Client() && boundary.workerMask == 0, "prepare lazy creation");
  boundary.lensReady = boundary.tapReady = boundary.sessionReady = true;
  check(producerCreates() == 3 && boundary.workerMask == kAllWorkers,
        "a producer creates each missing worker on the published runtime");
  check(deinitG2Client() && boundary.workerMask == 0, "the stop releases lazily created workers too");
  expectOff(); expectReleased();
  defaults();
}

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
  // The worker globals still read Ready with a queue and task, as after a
  // timed-out release: only the closed runtime refuses the submission.
  QueueHandle_t queue = &gR; TaskHandle_t task = &gR;
  check(!pageSwapRuntimeSnapshot(&queue, &task) && !queue && !task,
        "a kept page worker refuses submissions to a closed client");
  queue = &gR;
  check(!tapRuntimeSnapshot(&queue) && !queue && gUiSubmitters == 0,
        "a kept tap worker refuses a closed client without a claim");
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

int main() {
  // Before any scenario resets the runtime: the flag's boot value (spliced
  // from production) keeps the creation gate closed.
  check(producerCreates() == 0, "a fresh boot keeps the creation gate closed");
  transitions();
  memoryAdmission();
  workerCreationGate();
  workerAdmission();
  publicationAndConcurrency();
  std::cerr << "G2 lifecycle: " << assertions << " assertions passed\n";
}
