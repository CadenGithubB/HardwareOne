// The glasses, ring-retirement and ring clients on the one BLE setup worker.
// This unit runs the real worker (BLE_SetupWorker.cpp over
// BLE_SetupWorkerCore.h) on its own thread with the real client ops, the real
// g2SubmitBleConnect / g2SubmitRingRetire, the ring admission and submitters,
// g2Disconnect, g2RingDisconnect, g2RingYieldToPhonePipe and
// g2RingRetireForPhonePipe (G2_Glasses.cpp, G2_Ring.cpp) against the real
// BLE_CentralCoordinator.cpp. The phone is a fake client on the real
// runPhoneTurn. Time is virtual: the test advances the clock and waits for the
// worker to settle (parked on its wake, or blocked in a stub until the test
// lets it go), so no assertion depends on host timing.
// test_ring_phone_yield.py inserts the types and the production code.
#include "BLE_CentralCoordinator.h"
#include "BLE_CoexRecords.h"
#include "BLE_SetupWorkerCore.h"
#include "G2_RepairCore.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_BLE_PHONE_PIPE 1
#define ENABLE_MICROPHONE 1

static std::atomic<long> gChecks{0};
#define CHECK(x)                                                              \
  do {                                                                        \
    ++gChecks;                                                                \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x);       \
      std::fflush(stderr);                                                    \
      std::_Exit(1);                                                          \
    }                                                                         \
  } while (0)

// INSERT_TYPES

class String {
 public:
  String(const char* text = "") : text_(text ? text : "") {}
  void trim() {
    const auto first = text_.find_first_not_of(" \t\r\n");
    const auto last = text_.find_last_not_of(" \t\r\n");
    text_ = first == std::string::npos ? std::string() : text_.substr(first, last - first + 1);
  }
  unsigned length() const { return (unsigned)text_.size(); }
  const char* c_str() const { return text_.c_str(); }
 private:
  std::string text_;
};

// ---- Logs and the order of events ---------------------------------------------
static std::mutex gLogMutex;
static std::vector<std::string> gEvents, gLogs;
static void note(const std::string& event) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  gEvents.push_back(event);
}
static void hostLog(const char* level, const char* format, ...) {
  char text[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(gLogMutex);
  gLogs.push_back(std::string(level) + " " + text);
}
#define DEBUG_G2F(...) hostLog("D", __VA_ARGS__)
#define DEBUG_RING_LIFECYCLEF(...) hostLog("D", __VA_ARGS__)
#define INFO_RINGF(...) hostLog("I", __VA_ARGS__)
#define WARN_RINGF(...) hostLog("W", __VA_ARGS__)
#define ERROR_RINGF(...) hostLog("E", __VA_ARGS__)
#define ESP_LOGW(tag, ...) hostLog("W", __VA_ARGS__)
#define ESP_LOGE(tag, ...) hostLog("E", __VA_ARGS__)

// ---- FreeRTOS on a virtual clock -----------------------------------------------
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);
constexpr BaseType_t pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr BaseType_t APP_CORE = 1;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()

// The worker's wake is a binary semaphore. writeLock marks the ring's write
// mutex instead, which the test makes available or not (never waited for).
struct StaticSemaphore_t { int count = 0; bool writeLock = false; bool available = true; };
using SemaphoreHandle_t = StaticSemaphore_t*;
static unsigned gWakeCreates = 0;
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* s) { ++gWakeCreates; s->count = 0; return s; }

struct Shutdown {};    // the suite's end, thrown at the worker's wake
struct WorkerExit {};  // the worker's own vTaskDelete (bleSetupWorkerRelease)
static struct Sched {
  std::mutex m;
  std::condition_variable cv;
  uint64_t now = 1000;
  bool started = false, parked = false, hold = false, shutdown = false;
  uint64_t parkDeadline = 0;
  StaticSemaphore_t* parkSem = nullptr;
  bool (*stubPred)() = nullptr;  // the worker is in a stub until this holds
} gSched;
static std::atomic<uint32_t> gClock{1000};
static thread_local bool tlsWorker = false;
static uint32_t millis() { return gClock.load(); }

[[noreturn]] static void die(const char* what, int line) {
  std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
  std::fflush(stderr);
  std::_Exit(1);
}

// Only the worker sleeps on its wake. It parks until given, or until the
// virtual clock reaches the timeout; while the test holds it, it stays parked.
static int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ms) {
  if (s->writeLock) return s->available ? pdTRUE : pdFALSE;
  if (!tlsWorker) die("only the worker sleeps on its wake", __LINE__);
  std::unique_lock<std::mutex> lock(gSched.m);
  const uint64_t deadline = ms == portMAX_DELAY ? UINT64_MAX : gSched.now + ms;
  for (;;) {
    if (gSched.shutdown) { gSched.parked = false; throw Shutdown{}; }
    if (!gSched.hold) {
      if (s->count) { s->count = 0; gSched.parked = false; return pdTRUE; }
      if (gSched.now >= deadline) { gSched.parked = false; return pdFALSE; }
    }
    gSched.parked = true; gSched.parkDeadline = deadline; gSched.parkSem = s;
    gSched.cv.notify_all();
    gSched.cv.wait(lock);
  }
}
static int xSemaphoreGive(SemaphoreHandle_t s) {
  if (s->writeLock) return pdTRUE;
  std::lock_guard<std::mutex> lock(gSched.m);
  s->count = 1;
  gSched.cv.notify_all();
  return pdTRUE;
}

// A stub on the worker that blocks until the test lets it go (pred).
static void stubWait(const char* what, bool (*pred)()) {
  if (!tlsWorker) die(what, __LINE__);
  std::unique_lock<std::mutex> lock(gSched.m);
  gSched.stubPred = pred;
  gSched.cv.notify_all();
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) die(what, __LINE__);
    gSched.cv.wait_for(lock, std::chrono::milliseconds(1));
  }
  gSched.stubPred = nullptr;
  gSched.cv.notify_all();
}
static bool quietLocked() {
  if (!gSched.started) return true;  // no worker: nothing to wait for
  if (gSched.stubPred) return !gSched.stubPred();
  if (!gSched.parked) return false;
  if (gSched.hold) return true;
  return gSched.parkSem->count == 0 && gSched.now < gSched.parkDeadline;
}
// Waits (at most 10 s of host time) until the worker has nothing to do until
// the test acts: parked with no wake pending and no timeout due, or blocked in
// a stub that is waiting for the test.
static void settle(int line) {
  std::unique_lock<std::mutex> lock(gSched.m);
  if (!gSched.started) return;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!quietLocked()) {
    if (std::chrono::steady_clock::now() > end) die("the worker did not settle", line);
    gSched.cv.wait_for(lock, std::chrono::milliseconds(1));
  }
}
static void advance(uint32_t ms, int line) {
  settle(line);
  {
    std::lock_guard<std::mutex> lock(gSched.m);
    gSched.now += ms;
    gClock = (uint32_t)gSched.now;
    gSched.cv.notify_all();
  }
  settle(line);
}
#define SETTLE() settle(__LINE__)
#define ADVANCE(ms) advance((ms), __LINE__)
// The worker is slow to wake: it stays parked through gives and timeouts.
static void hold() {
  SETTLE();
  std::lock_guard<std::mutex> lock(gSched.m);
  gSched.hold = true;
}
static void resume() {
  {
    std::lock_guard<std::mutex> lock(gSched.m);
    gSched.hold = false;
    gSched.cv.notify_all();
  }
  SETTLE();
}
static std::atomic<unsigned> gTestDelays{0};
// Runs once, on the test task's next delay: what the world does while a
// command task waits inside the worker API (bleSetupWorkerRelease).
static std::atomic<void (*)()> gOnTestDelay{nullptr};
static void vTaskDelay(TickType_t ms) {
  if (tlsWorker) die("the worker never delays here", __LINE__);
  ++gTestDelays;
  if (void (*hook)() = gOnTestDelay.exchange(nullptr)) hook();
  ADVANCE(ms);
}

static std::thread gWorker;
static int gWorkerTask = 0, gOtherTask = 0;
static std::string gWorkerName;
static uint32_t gWorkerStack = 0;
static UBaseType_t gWorkerPriority = 0;
static BaseType_t gWorkerCore = -1;
static BaseType_t xTaskCreateLogged(TaskFunction_t fn, const char* name, const uint32_t stack, void* arg,
                                    UBaseType_t priority, TaskHandle_t* handle, const char* tag,
                                    BaseType_t core) {
  (void)tag;
  CHECK(!gWorker.joinable());
  gWorkerName = name; gWorkerStack = stack; gWorkerPriority = priority; gWorkerCore = core;
  {
    std::lock_guard<std::mutex> lock(gSched.m);
    gSched.started = true;
  }
  gWorker = std::thread([fn, arg] {
    tlsWorker = true;
    try { fn(arg); } catch (const Shutdown&) {} catch (const WorkerExit&) {}
  });
  if (handle) *handle = &gWorkerTask;
  return pdPASS;
}
static TaskHandle_t xTaskGetCurrentTaskHandle() { return tlsWorker ? (void*)&gWorkerTask : (void*)&gOtherTask; }
// The worker's cooperative exit: on hardware the task function never returns,
// so the thread ends here; the test joins it (idle reaps the TCB there).
[[noreturn]] static void vTaskDelete(TaskHandle_t handle) {
  if (!tlsWorker || handle != nullptr) die("only the worker deletes itself", __LINE__);
  {
    std::lock_guard<std::mutex> lock(gSched.m);
    gSched.started = false; gSched.parked = false;
    gSched.cv.notify_all();
  }
  throw WorkerExit{};
}

// ---- Radio, roles, lifecycle ---------------------------------------------------
static std::atomic<uint8_t> gLinks{3};
uint8_t radioHalCompanionBleLinks() { return gLinks.load(); }
static std::atomic<BleRoleTransition> gRole{BleRoleTransition::IDLE};
// Runs once, on the worker, at the role check that ends a client's shared
// ready checks: a cancel or fault that lands after ready() said Run.
static std::atomic<void (*)()> gAfterWorkerRoleCheck{nullptr};
static BleRoleTransition bleRoleTransitionState() {
  if (tlsWorker) {
    if (void (*hook)() = gAfterWorkerRoleCheck.exchange(nullptr)) hook();
  }
  return gRole.load();
}
static bool bleRoleTransitionHeldByCurrentTask() { return false; }
static std::atomic<bool> gLifecycleFaulted{false};
static bool bleStackLifecycleFaulted() { return gLifecycleFaulted.load(); }
static bool isBleServerInitialized() { return false; }
// The shared client stack (BLE_CentralStack.cpp) is up throughout; the ring owner is open.
static bool bleStackReady() { return true; }
static bool g2RingIsOpen() { return true; }
enum SysEvt : uint8_t { SYSEVT_G2_DISCONNECTED, SYSEVT_RING_RECONNECT_FAILED };
static void systemEventPost(SysEvt, const char*, const char*) {}

// ---- What a job's connect records see -------------------------------------------
static constexpr uint32_t kNoWait = UINT32_MAX;
static std::atomic<uint32_t> gAccessoryWait{kNoWait};
struct SetupWaitLog { std::string label; uint32_t ms; };
static std::vector<SetupWaitLog> gSetupWaits;
void bleCoexSetAccessoryWait(uint32_t ms) {
  CHECK(tlsWorker && gAccessoryWait.load() == kNoWait);
  gAccessoryWait = ms;
}
void bleCoexClearAccessoryWait() { CHECK(tlsWorker); gAccessoryWait = kNoWait; }
void bleCoexLogSetupWait(const char* job, uint32_t ms) {
  CHECK(tlsWorker && job && job[0]);
  std::lock_guard<std::mutex> lock(gLogMutex);
  gSetupWaits.push_back({job, ms});
}

// INSERT_WORKER_API

// ---- Peers -----------------------------------------------------------------------
static std::atomic<bool> gRequestCurrent{true}, gIntentCurrent{true};
static bool blePeerConnectRequestIsCurrent(BlePeerKind kind, const BlePeerConnectRequest&) {
  if (kind != BLE_PEER_R1_RING) die("ring requests only", __LINE__);  // per ready(): not counted
  return gRequestCurrent.load();
}
static bool blePeerIntentIsCurrent(BlePeerKind kind, uint32_t, uint32_t) {
  if (kind != BLE_PEER_R1_RING) die("ring requests only", __LINE__);  // per ready(): not counted
  return gIntentCurrent.load();
}
static bool blePeerBeginManualLearn(BlePeerKind, uint32_t& intent, uint32_t& identity) {
  intent = 1; identity = 1;
  return true;
}
// As BLE_Peers.cpp: a manual saved-target request is a one-shot explicit reseek.
static bool blePeerBeginManualConnectRequest(BlePeerKind kind, BlePeerConnectRequest& request) {
  CHECK(kind == BLE_PEER_R1_RING);
  request = BlePeerConnectRequest{};
  request.intentGeneration = 3;
  request.identityGeneration = 4;
  request.autoReconnect = false;
  request.explicitReseek = true;
  std::strcpy(request.savedTarget.mac1, "aa:bb:cc:dd:ee:03");
  return true;
}
bool g2RingConnectInFlight();
struct Restore { uint32_t intent, identity; bool inFlight, onWorker; };
static std::vector<Restore> gRestores;
[[maybe_unused]] static bool blePeerRestoreDroppedReseek(BlePeerKind kind, uint32_t intent, uint32_t identity) {
  CHECK(kind == BLE_PEER_R1_RING);
  // Rule R runs before the in-flight flag is released.
  const Restore r{intent, identity, g2RingConnectInFlight(), tlsWorker};
  std::lock_guard<std::mutex> lock(gLogMutex);
  gRestores.push_back(r);
  return true;
}
static void blePeerNoteUserDisconnect(BlePeerKind) {}
static void blePeerNoteLinkLost(BlePeerKind) {}

// ---- Ring module boundary ----------------------------------------------------------
static unsigned gRingInits = 0;
static bool g2RingInit() { ++gRingInits; return true; }
static bool ringPublishedConnected() { return false; }
// The ring's completion mutex (recursive, as in production).
static std::recursive_mutex gRingCompletion;
struct RingCompletionGuard {
  RingCompletionGuard() { gRingCompletion.lock(); }
  ~RingCompletionGuard() { gRingCompletion.unlock(); }
  explicit operator bool() const { return true; }
};
static String gRingDeviceName;
struct FakeRingClient {
  bool connected = false;
  unsigned disconnects = 0;
  bool isConnected() const { return connected; }
  void disconnect() { ++disconnects; connected = false; }
};
static FakeRingClient gRingClient;
static struct {
  FakeRingClient* client = nullptr;
  SemaphoreHandle_t writeMutex = nullptr;
  bool clientStale = false;
} gRing;
constexpr unsigned BLE_CLIENT_RETIRE_R1 = 2;
struct RetireCall { uint32_t wait; bool onWorker; };
static std::vector<RetireCall> gRetires;
static bool bleRetireClientForReplacement(FakeRingClient*& client, unsigned slot, uint8_t peer, const char* why) {
  CHECK(slot == BLE_CLIENT_RETIRE_R1 && peer == BLE_PEER_R1_RING && !std::strcmp(why, "RING-YIELD"));
  const RetireCall call{gAccessoryWait.load(), tlsWorker};
  {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gRetires.push_back(call);
  }
  note("retire ring client");
  client = nullptr;
  return true;
}
static bool bleCentralTxTake(uint32_t) { return true; }
static void bleCentralTxGive() {}
static void ringClearGattPointers(bool) {}
static void ringConnectCancelAdvanceLocked();
static void ringBeginLogicalDown(const char*, bool userInitiated = false) {
  RingCompletionGuard completion;
  ringConnectCancelAdvanceLocked();
  note(userInitiated ? "ring down (user)" : "ring down");
}

// The ring's connect and scan bodies. A blocked body stands for a long setup
// in progress; a stop request (g2RingScanStop) or the test ends it.
struct ConnectCall {
  std::string mac;
  bool fenced;
  BlePeerConnectRequest request;
  uint32_t gen, wait;
  bool onWorker;
};
static std::vector<ConnectCall> gConnects;
static std::atomic<bool> gBlockRing{false}, gReleaseRing{false}, gScanStopRequested{false};
static std::atomic<unsigned> gScanStops{0};
static bool ringBodyDone() { return gReleaseRing.load() || gScanStopRequested.load(); }
bool ringPerformConnect(const String& mac, const BlePeerConnectRequest* expected, uint32_t gen) {
  const ConnectCall call{mac.c_str(), expected != nullptr, expected ? *expected : BlePeerConnectRequest{},
                         gen, gAccessoryWait.load(), tlsWorker};
  {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gConnects.push_back(call);
  }
  note("ring connect");
  if (gBlockRing) stubWait("ring connect body", ringBodyDone);
  return true;
}
struct ScanCall { uint32_t seconds, gen, wait; bool endWhenOtherQueued, otherQueuedAtEnd; };
static std::vector<ScanCall> gScans;
bool g2RingScan(uint32_t seconds, uint32_t gen, bool endWhenOtherQueued) {
  note("ring scan");
  if (gBlockRing) stubWait("ring scan body", ringBodyDone);
  const ScanCall call{seconds, gen, gAccessoryWait.load(), endWhenOtherQueued,
                      bleSetupOtherQueued(BleSetupClient::Ring)};
  std::lock_guard<std::mutex> lock(gLogMutex);
  gScans.push_back(call);
  return true;
}
void g2RingScanStop() { ++gScanStops; gScanStopRequested = true; note("ring scan stop"); }
void g2RingScanStopReset() { gScanStopRequested = false; note("ring scan stop reset"); }

// G2 audio.
static std::atomic<bool> gAudioCritical{false};
static std::atomic<unsigned> gAudioWaitNotes{0};
static std::vector<uint32_t> gAudioEnded, gAudioExpired;
bool g2RingAudioThroughputCritical() { return gAudioCritical.load(); }
void g2RingNoteAudioWait() { CHECK(tlsWorker); ++gAudioWaitNotes; }
void g2RingNoteAudioWaitEnded(uint32_t ms) {
  CHECK(tlsWorker);
  note("audio wait ended");
  std::lock_guard<std::mutex> lock(gLogMutex);
  gAudioEnded.push_back(ms);
}
void g2RingNoteAudioWaitExpired(uint32_t ms) {
  CHECK(tlsWorker);
  std::lock_guard<std::mutex> lock(gLogMutex);
  gAudioExpired.push_back(ms);
}

// ---- Glasses boundary --------------------------------------------------------------
struct G2Call { std::string what; uint32_t epoch, wait; bool fenced; uint8_t attempt; bool onWorker; };
static std::vector<G2Call> gG2Calls;
static void noteG2(const G2Call& call) {
  {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gG2Calls.push_back(call);
  }
  note(call.what);
}
static bool g2ConnectSync(G2Eye eye, const BlePeerConnectRequest* expected, const BlePeerSavedTarget* filter,
                          uint32_t epoch) {
  CHECK(eye == G2_EYE_RIGHT && !filter);
  noteG2({"g2 eye", epoch, gAccessoryWait.load(), expected != nullptr, 0, tlsWorker});
  return true;
}
static std::atomic<bool> gRepairResult{true};
static bool attemptMissingArmRecoverySync(uint8_t attempt, const BleConnectJob& job) {
  noteG2({"g2 repair", job.cancelEpoch, gAccessoryWait.load(), false, attempt, tlsWorker});
  return gRepairResult.load();
}
struct RepairComplete { bool ok, onWorker; };
static std::vector<RepairComplete> gRepairCompletes;
static void missingArmRecoveryComplete(bool ok) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  gRepairCompletes.push_back({ok, tlsWorker});
}
struct G2CompletionGuard { explicit operator bool() const { return true; } };
static uint64_t g2EvenAiCurrentExchangeId() { return 0; }
static void g2EvenAiTerminate(uint64_t, const char*) {}
// Read only on g2Disconnect's in-flight cancel path.
static std::atomic<unsigned> gG2StateReads{0};
static G2State getG2State() { ++gG2StateReads; return G2_STATE_CONNECTING; }
static void g2ScanStopIfActive() {}
struct FakeTempleClient {
  std::atomic<unsigned> disconnects{0};
  bool isConnected() const { return true; }
  void disconnect() { ++disconnects; }
};
struct G2Temple { FakeTempleClient* client; };
static FakeTempleClient gLeftClient, gRightClient;
static G2Temple gL{&gLeftClient}, gR{&gRightClient};
static void stopHeartbeatTimer() {}
static bool g2ControlWorkerShutdown() { return true; }
static void disconnectTemple(G2Temple&) {}
static void g2ClientSetState(G2State) {}
static void g2PushStatusEvent(const char*) {}

void g2RingWithdrawQueuedConnect();
void g2CancelActiveRingScan();
bool g2SubmitBleConnect(const BleConnectJob& job);
bool g2SubmitRingRetire();

// INSERT_PRODUCTION

// A G2_SAVED body; blocked, it stands for a glasses setup in progress, which
// ends when the test lets it go or its epoch is cancelled.
// g2Disconnect's forced temple disconnect (after it advanced the epoch) is
// what unblocks a running body on hardware, so that is what ends it here.
static std::atomic<bool> gBlockG2{false}, gReleaseG2{false};
static std::atomic<uint32_t> gBlockedG2Epoch{0};
static std::atomic<unsigned> gBlockedG2Disconnects{0};
static std::atomic<void (*)()> gDuringG2Saved{nullptr};
static bool g2BodyDone() {
  return gReleaseG2.load() || (gBlockedG2Epoch.load() != bleConnectCancelEpochSnapshot() &&
                               gLeftClient.disconnects.load() != gBlockedG2Disconnects.load());
}
static bool g2ConnectSavedSync(const BleConnectJob& job) {
  noteG2({"g2 saved", job.cancelEpoch, gAccessoryWait.load(), false, 0, tlsWorker});
  if (void (*during)() = gDuringG2Saved.load()) during();
  if (gBlockG2) {
    gBlockedG2Epoch = job.cancelEpoch;
    gBlockedG2Disconnects = gLeftClient.disconnects.load();
    stubWait("g2 saved body", g2BodyDone);
  }
  return true;
}

// Every client's ops pass through a spy: what the worker asked and when.
struct SpyFinish { hw1setup::End end; uint8_t reason; uint32_t audioMs; bool onWorker; };
struct Spy {
  const char* name = "";
  const BleSetupOps* real = nullptr;
  std::atomic<unsigned> readies{0};
  std::vector<uint8_t> waits;
  std::vector<hw1setup::RunInfo> runs;
  std::vector<SpyFinish> finishes;
};
enum : int { kSpyG2 = 0, kSpyRetire = 1, kSpyRing = 2, kSpyPhone = 3 };
static Spy gSpy[4];
static BleSetupOps gSpyOps[4];

// INSERT_WORKER

// Forced publish failures (a publish that loses its claim).
static std::atomic<bool> gFailPublish{false};
bool bleSetupPublish(BleSetupClient client, uint32_t token) {
  if (gFailPublish.exchange(false)) return false;
  return realBleSetupPublish(client, token);
}

// ---- The fake phone, on the real runPhoneTurn ---------------------------------------
static hw1setup::PhoneTurn gTurn;
static std::atomic<uint32_t> gPhoneWant{0};
static std::atomic<uint32_t> gPhoneRunWait{0};
static std::atomic<unsigned> gPhoneGrants{0}, gPhoneReleases{0};
struct PhoneTurnEnv {
  uint32_t now() { return millis(); }
  void waitWake(uint32_t ms) { bleSetupWaitWake(ms); }
  void grant(uint32_t token) { ++gPhoneGrants; gTurn.granted.store(token); }
  void release(uint32_t) { ++gPhoneReleases; }
  void overran(uint32_t, bool) { note("phone overran"); }
  void escalate(uint32_t) { bleCentralRecoveryBlock(true); }
  void settle(uint32_t) { bleCentralRecoveryBlock(false); }
};
static const char* phoneLabel(uint32_t) { return "phone"; }
static hw1setup::Verdict phoneReady(uint32_t token, const hw1setup::WaitInfo&) {
  if (gPhoneWant.load() != token) return {hw1setup::Ready::Drop, hw1setup::kInactive};
  if (bleCentralRecoveryBlocked()) return {hw1setup::Ready::Wait, hw1setup::kRecovery};
  return {hw1setup::Ready::Run, hw1setup::kNone};
}
static hw1setup::End phoneRun(uint32_t token, const hw1setup::RunInfo&) {
  gPhoneRunWait = gAccessoryWait.load();
  PhoneTurnEnv env;
  return hw1setup::runPhoneTurn(gTurn, token, env);
}
static void phoneFinish(uint32_t, const hw1setup::FinishInfo&) {}
extern const BleSetupOps phonePipeSetupOps = {phoneLabel, phoneReady, nullptr, phoneRun, phoneFinish, false};

// The phone task's side: queue a setup (any thread), take the granted turn,
// give it back.
static uint32_t phoneQueue() {
  uint32_t token = 0;
  CHECK(bleSetupClaim(BleSetupClient::Phone, &token));
  gPhoneWant = token;
  CHECK(bleSetupPublish(BleSetupClient::Phone, token));
  return token;
}
static uint32_t phoneTakeTurn() {
  const uint32_t token = phoneQueue();
  SETTLE();
  CHECK(gTurn.granted.load() == token && hw1setup::consumeGrant(gTurn, token));
  return token;
}
static void phoneEndTurn(uint32_t token) {
  CHECK(hw1setup::grantHeld(gTurn, token));
  gPhoneWant = 0;
  gTurn.ended.store(token);
  bleSetupWake();
  SETTLE();
  CHECK(gTurn.granted.load() != (token | hw1setup::kUsed));
}

// ---- Spies ---------------------------------------------------------------------------
static const char* const kSpyNames[4] = {"G2", "RingRetire", "Ring", "Phone"};
static const char* endName(hw1setup::End end) {
  switch (end) {
    case hw1setup::End::Ran: return "Ran";
    case hw1setup::End::Dropped: return "Dropped";
    case hw1setup::End::Withdrawn: return "Withdrawn";
    case hw1setup::End::Overrun: return "Overrun";
  }
  return "?";
}
template <int I> const char* spyLabel(uint32_t token) { return gSpy[I].real->label(token); }
template <int I> hw1setup::Verdict spyReady(uint32_t token, const hw1setup::WaitInfo& info) {
  if (!tlsWorker) die("ready() off the worker", __LINE__);
  ++gSpy[I].readies;
  return gSpy[I].real->ready(token, info);
}
template <int I> void spyWaiting(uint32_t token, uint8_t reason) {
  {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gSpy[I].waits.push_back(reason);
  }
  if (gSpy[I].real->waiting) gSpy[I].real->waiting(token, reason);
}
template <int I> hw1setup::End spyRun(uint32_t token, const hw1setup::RunInfo& info) {
  CHECK(tlsWorker);
  {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gSpy[I].runs.push_back(info);
  }
  note(std::string("run ") + kSpyNames[I]);
  return gSpy[I].real->run(token, info);
}
template <int I> void spyFinish(uint32_t token, const hw1setup::FinishInfo& info) {
  gSpy[I].real->finish(token, info);
  const SpyFinish f{info.end, info.reason, info.audioMs, tlsWorker};
  {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gSpy[I].finishes.push_back(f);
  }
  note(std::string("finish ") + kSpyNames[I] + " " + endName(info.end));
}
template <int I> void installSpy(const BleSetupOps& real) {
  gSpy[I].name = kSpyNames[I];
  gSpy[I].real = &real;
  // waiting is always spied (Env::waiting skips a null one, so this only records).
  gSpyOps[I] = BleSetupOps{spyLabel<I>, spyReady<I>, spyWaiting<I>, spyRun<I>, spyFinish<I>, real.recordWait};
}

static size_t finishCount(int i) { std::lock_guard<std::mutex> l(gLogMutex); return gSpy[i].finishes.size(); }
static size_t runCount(int i) { std::lock_guard<std::mutex> l(gLogMutex); return gSpy[i].runs.size(); }
static SpyFinish lastFinish(int i) {
  std::lock_guard<std::mutex> l(gLogMutex);
  CHECK(!gSpy[i].finishes.empty());
  return gSpy[i].finishes.back();
}
static hw1setup::RunInfo lastRun(int i) {
  std::lock_guard<std::mutex> l(gLogMutex);
  CHECK(!gSpy[i].runs.empty());
  return gSpy[i].runs.back();
}
static std::vector<uint8_t> waitsOf(int i) { std::lock_guard<std::mutex> l(gLogMutex); return gSpy[i].waits; }
static uint8_t lastWait(int i) {
  std::lock_guard<std::mutex> l(gLogMutex);
  CHECK(!gSpy[i].waits.empty());
  return gSpy[i].waits.back();
}
template <class T> static std::vector<T> copyOf(const std::vector<T>& v) {
  std::lock_guard<std::mutex> l(gLogMutex);
  return v;
}
template <class T> static T lastOf(const std::vector<T>& v, int line) {
  std::lock_guard<std::mutex> l(gLogMutex);
  if (v.empty()) die("nothing recorded", line);
  return v.back();
}
#define LAST(v) lastOf((v), __LINE__)
static void clearEvents() { std::lock_guard<std::mutex> l(gLogMutex); gEvents.clear(); }
static std::vector<std::string> events() { return copyOf(gEvents); }
// The events among `wanted`, in the order they happened.
static std::vector<std::string> eventsOf(std::initializer_list<const char*> wanted) {
  std::vector<std::string> out;
  for (const auto& e : events())
    for (const char* w : wanted)
      if (e == w) out.push_back(e);
  return out;
}
static size_t eventAt(const std::string& wanted) {
  const auto all = events();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i] == wanted) return i;
  die(wanted.c_str(), __LINE__);
}
static bool logged(const char* needle) {
  for (const auto& line : copyOf(gLogs))
    if (line.find(needle) != std::string::npos) return true;
  return false;
}
static size_t setupWaitCount() { std::lock_guard<std::mutex> l(gLogMutex); return gSetupWaits.size(); }
static SetupWaitLog lastSetupWait() {
  std::lock_guard<std::mutex> l(gLogMutex);
  CHECK(!gSetupWaits.empty());
  return gSetupWaits.back();
}
static size_t connectCount() { std::lock_guard<std::mutex> l(gLogMutex); return gConnects.size(); }
static ConnectCall lastConnect() {
  std::lock_guard<std::mutex> l(gLogMutex);
  CHECK(!gConnects.empty());
  return gConnects.back();
}
static size_t scanCount() { std::lock_guard<std::mutex> l(gLogMutex); return gScans.size(); }
static ScanCall lastScan() {
  std::lock_guard<std::mutex> l(gLogMutex);
  CHECK(!gScans.empty());
  return gScans.back();
}
static size_t g2CallCount() { std::lock_guard<std::mutex> l(gLogMutex); return gG2Calls.size(); }
static G2Call lastG2Call() {
  std::lock_guard<std::mutex> l(gLogMutex);
  CHECK(!gG2Calls.empty());
  return gG2Calls.back();
}
static size_t restoreCount() { std::lock_guard<std::mutex> l(gLogMutex); return gRestores.size(); }
static size_t repairCount() { std::lock_guard<std::mutex> l(gLogMutex); return gRepairCompletes.size(); }
static RepairComplete lastRepair() {
  std::lock_guard<std::mutex> l(gLogMutex);
  CHECK(!gRepairCompletes.empty());
  return gRepairCompletes.back();
}
static size_t retireCount() { std::lock_guard<std::mutex> l(gLogMutex); return gRetires.size(); }

// A probe claim: tokens are handed out in sequence, so the gap between two
// probes counts the claims made in between (the phone slot must be free).
static uint32_t probeToken() {
  uint32_t token = 0;
  CHECK(bleSetupClaim(BleSetupClient::Phone, &token));
  bleSetupCancelClaim(BleSetupClient::Phone, token);
  return token;
}

// ---- Producers -------------------------------------------------------------------------
static bool gConnectActive() { return gConnectTaskActive; }
static bool submitG2(BleConnectKind kind, bool epochZero = false) {
  uint32_t epoch = 0;
  CHECK(g2ConnectClaim(&epoch) && epoch != 0);
  BleConnectJob job{};
  job.kind = kind;
  job.cancelEpoch = epochZero ? 0 : epoch;
  if (kind == BleConnectKind::G2_EYE) {
    job.eye = (uint8_t)G2_EYE_RIGHT;
    job.peerIntentGeneration = 1;
    job.peerIdentityGeneration = 1;
  }
  if (g2SubmitBleConnect(job)) return true;
  g2ConnectRelease();
  return false;
}
static BlePeerConnectRequest savedRing(bool autoReconnect = true, bool explicitReseek = false) {
  BlePeerConnectRequest request;
  request.autoReconnect = autoReconnect;
  request.explicitReseek = explicitReseek;
  request.intentGeneration = 5;
  request.identityGeneration = 6;
  std::strcpy(request.savedTarget.mac1, "aa:bb:cc:dd:ee:01");
  return request;
}
static bool ringSaved() { return ringPeerConnectSavedAdmissionThunk(savedRing()) == BlePeerConnectAdmission::STARTED; }
static bool ringNudge() {  // a one-shot explicit reseek
  return ringPeerConnectSavedAdmissionThunk(savedRing(false, true)) == BlePeerConnectAdmission::STARTED;
}
static bool ringAutoNudge() {  // an explicit reseek on an auto-reconnect ring
  return ringPeerConnectSavedAdmissionThunk(savedRing(true, true)) == BlePeerConnectAdmission::STARTED;
}
static bool ringManualSaved() { return g2RingConnectSaved(); }
static bool ringScanConnect() { return g2RingConnect(); }
static bool ringMac() { return g2RingConnectMac(String("aa:bb:cc:dd:ee:02")); }
static bool ringScanOnly() { return g2RingScanAsync(5); }
static bool g2Saved() { return submitG2(BleConnectKind::G2_SAVED); }
static bool g2Repair() { return submitG2(BleConnectKind::G2_REPAIR); }

static uint32_t ringGeneration() {
  portENTER_CRITICAL(&gRingConnectMux);
  const uint32_t generation = gRingConnectCancelGeneration;
  portEXIT_CRITICAL(&gRingConnectMux);
  return generation;
}
static void ringCancel() { RingCompletionGuard completion; ringConnectCancelAdvanceLocked(); }

static void resetWorld() {
  SETTLE();
  gRole = BleRoleTransition::IDLE;
  gLifecycleFaulted = false;
  gBleClientRetirementPeerMask = 0;
  bleCentralRecoveryBlock(false);
  gRequestCurrent = true;
  gIntentCurrent = true;
  gAudioCritical = false;
  bleCentralPhoneEnabled(false);
  gLinks = 3;
  gBlockRing = false; gReleaseRing = false; gScanStopRequested = false;
  gBlockG2 = false; gReleaseG2 = false;
  gDuringG2Saved = nullptr;
  gRing.client = nullptr;
  SETTLE();
  CHECK(bleSetupIdle(hw1setup::kAll) && !gConnectActive() && !g2RingConnectInFlight());
}

// ---- Cases -----------------------------------------------------------------------------

// The job carries the peer request through the worker unchanged.
static void requestRoundTrip() {
  BlePeerConnectRequest request = savedRing(false, true);
  std::strcpy(request.savedTarget.mac2, "aa:bb:cc:dd:ee:09");
  request.savedTarget.addressType1 = 1;
  request.savedTarget.addressType1Known = true;
  BleConnectJob job{};
  blePeerRequestToJob(request, job);
  CHECK(bleJobHasPeerRequest(job) && !bleJobHasManualFence(job));
  const BlePeerConnectRequest back = blePeerRequestFromJob(job);
  CHECK(back.intentGeneration == 5 && back.identityGeneration == 6 && back.explicitReseek && !back.autoReconnect);
  CHECK(!std::strcmp(back.savedTarget.mac1, "aa:bb:cc:dd:ee:01") && !std::strcmp(back.savedTarget.mac2, "aa:bb:cc:dd:ee:09"));
  CHECK(back.savedTarget.addressType1 == 1 && back.savedTarget.addressType1Known && !back.savedTarget.addressType2Known);
}

// Pipe off: every ring path queues work and the worker runs it, recording the
// job's wait on its connect records and clearing it after.
static void pipeOffRingPathsRun() {
  CHECK(!bleCentralRingShouldYield());
  struct Path { bool (*submit)(); const char* label; const char* mac; bool fenced; };
  const Path paths[] = {
      {ringSaved, "ring saved", "aa:bb:cc:dd:ee:01", true},
      {ringScanConnect, "ring scan", "", true},
      {[] { return g2RingConnectMac(String(" aa:bb:cc:dd:ee:02 ")); }, "ring mac", "aa:bb:cc:dd:ee:02", true},
      {ringManualSaved, "ring saved", "aa:bb:cc:dd:ee:03", true},
  };
  for (const Path& path : paths) {
    const size_t connects = connectCount(), finishes = finishCount(kSpyRing), waits = setupWaitCount();
    const uint32_t gen = ringGeneration();
    CHECK(path.submit());
    SETTLE();
    CHECK(connectCount() == connects + 1 && finishCount(kSpyRing) == finishes + 1);
    const ConnectCall call = lastConnect();
    CHECK(call.mac == path.mac && call.fenced == path.fenced && call.gen == gen && call.onWorker);
    CHECK(call.wait == 0);                                   // recorded (nothing held it up)
    CHECK(gAccessoryWait.load() == kNoWait);                 // and cleared after
    CHECK(setupWaitCount() == waits + 1 && lastSetupWait().label == path.label);
    CHECK(lastFinish(kSpyRing).end == hw1setup::End::Ran && !g2RingConnectInFlight());
  }
  // The saved reconnect carries its request; the manual kinds their fence.
  const auto connects = copyOf(gConnects);
  const ConnectCall saved = connects[connects.size() - 4], scan = connects[connects.size() - 3];
  CHECK(saved.request.autoReconnect && saved.request.intentGeneration == 5 && saved.request.identityGeneration == 6);
  CHECK(!scan.request.autoReconnect && scan.request.intentGeneration == 1 && scan.request.identityGeneration == 1);
  const ConnectCall manualSaved = connects.back();
  CHECK(manualSaved.request.explicitReseek && !manualSaved.request.autoReconnect);
  CHECK(manualSaved.request.intentGeneration == 3 && manualSaved.request.identityGeneration == 4);
  const size_t scans = scanCount();
  CHECK(g2RingScanAsync(30));
  SETTLE();
  CHECK(scanCount() == scans + 1);
  const ScanCall manual = lastScan();
  CHECK(manual.seconds == 30 && manual.endWhenOtherQueued && manual.gen == ringGeneration() && manual.wait == 0);
  CHECK(lastSetupWait().label == "ring scan-only" && !g2RingConnectInFlight());
  CHECK(copyOf(gAudioEnded).empty());  // no audio wait, no "audio idle" line
}

// Every ring path while the ring must yield: refused before any claim, init
// or in-flight flag.
static void ringPathsRefuse() {
  CHECK(bleCentralRingShouldYield());
  const unsigned inits = gRingInits, readies = gSpy[kSpyRing].readies;
  const uint32_t before = probeToken();
  CHECK(ringPeerConnectSavedAdmissionThunk(savedRing()) == BlePeerConnectAdmission::ROLE_BLOCKED);
  CHECK(!g2RingConnect() && !g2RingConnectSaved() && !g2RingConnectMac(String("aa:bb:cc:dd:ee:02")) &&
        !g2RingScanAsync(30));
  SETTLE();
  CHECK(probeToken() == before + 1);  // no claim in between
  CHECK(!g2RingConnectInFlight() && gRingInits == inits && gSpy[kSpyRing].readies == readies);
  CHECK(bleSetupIdle(hw1setup::kAll));
}

static void refusalsAndFourLinks() {
  bleCentralPhoneEnabled(true);
  ringPathsRefuse();
  // A four-link radio keeps the ring beside the pipe.
  gLinks = 4;
  CHECK(!bleCentralRingShouldYield());
  const size_t connects = connectCount();
  CHECK(ringSaved());
  SETTLE();
  CHECK(connectCount() == connects + 1 && lastFinish(kSpyRing).end == hw1setup::End::Ran);
  // Back to three links, then the pipe stops: the ring returns.
  gLinks = 3;
  ringPathsRefuse();
  bleCentralPhoneEnabled(false);
  CHECK(ringScanConnect());
  SETTLE();
  CHECK(connectCount() == connects + 2);
  resetWorld();
}

// Ring jobs of every kind queued before the pipe started are dropped (kYield)
// and never run; a glasses job beside each still runs. Rule R: only a
// one-shot reseek (a RING_SAVED with explicitReseek and no autoReconnect: the
// scheduler's nudge or a manual saved connect) is put back, with its own
// generations, before the in-flight flag is released.
static void queuedRingJobsYield() {
  struct Kind { bool (*submit)(); bool restores; uint32_t intent, identity; };
  const Kind kinds[] = {{ringSaved, false, 0, 0}, {ringNudge, true, 5, 6}, {ringAutoNudge, false, 0, 0},
                        {ringManualSaved, true, 3, 4}, {ringScanConnect, false, 0, 0}, {ringMac, false, 0, 0},
                        {ringScanOnly, false, 0, 0}};
  for (const Kind& kind : kinds) {
    bleCentralPhoneEnabled(false);
    hold();
    CHECK(kind.submit() && g2Saved());
    bleCentralPhoneEnabled(true);
    const size_t connects = connectCount(), scans = scanCount(), restores = restoreCount();
    const size_t ringRuns = runCount(kSpyRing), g2Runs = runCount(kSpyG2), finishes = finishCount(kSpyRing);
    resume();
    CHECK(finishCount(kSpyRing) == finishes + 1 && runCount(kSpyRing) == ringRuns);
    const SpyFinish f = lastFinish(kSpyRing);
    CHECK(f.end == hw1setup::End::Dropped && f.reason == hw1setup::kYield && f.onWorker);
    CHECK(connectCount() == connects && scanCount() == scans);
    CHECK(runCount(kSpyG2) == g2Runs + 1 && lastFinish(kSpyG2).end == hw1setup::End::Ran);
    CHECK(!g2RingConnectInFlight() && !gConnectActive());
    CHECK(restoreCount() == restores + (kind.restores ? 1 : 0));
    if (kind.restores) {
      const Restore r = LAST(gRestores);
      CHECK(r.intent == kind.intent && r.identity == kind.identity && r.inFlight && r.onWorker);
    }
  }
  CHECK(logged("ring yields to the enabled phone pipe"));
  resetWorld();
}

// The worker's family flags: the G2 job's gConnectTaskActive (cleared by
// g2ConnectRelease) and the ring's gRingConnectTaskActive (cleared by
// g2RingConnectMarkComplete).
static bool familyFlag(int spy) { return spy == kSpyG2 ? gConnectActive() : g2RingConnectInFlight(); }

// A queued job held in a role-transition Wait ends at the next 100 ms recheck
// once its event lands: exactly one finish (Dropped, reason), no run, and its
// family flag released.
static void endsAtRecheck(int spy, bool (*submit)(), void (*event)(), uint8_t reason, bool restores, int line) {
  hold();
  if (!submit()) die("submit", line);
  gRole = BleRoleTransition::G2_STOP;
  const size_t finishes = finishCount(spy), runs = runCount(spy), restored = restoreCount();
  const size_t waits = waitsOf(spy).size();
  resume();
  if (!(waitsOf(spy).size() == waits + 1 && lastWait(spy) == hw1setup::kRole)) die("role wait", line);
  ADVANCE(300);
  if (!(finishCount(spy) == finishes && runCount(spy) == runs && familyFlag(spy))) die("waits", line);
  event();
  ADVANCE(99);
  if (finishCount(spy) != finishes || !familyFlag(spy)) die("ended before the recheck", line);
  ADVANCE(1);
  if (finishCount(spy) != finishes + 1) die("one finish at the recheck", line);
  const SpyFinish f = lastFinish(spy);
  if (!(f.end == hw1setup::End::Dropped && f.reason == reason && f.onWorker)) die("dropped with its reason", line);
  if (runCount(spy) != runs || familyFlag(spy)) die("no run, flag released", line);
  if (restoreCount() != restored + (restores ? 1 : 0)) die("rule R", line);
  if (restores && !LAST(gRestores).inFlight) die("restore before MarkComplete", line);
  resetWorld();
}

static void cancelsEndQueuedJobs() {
  using namespace hw1setup;
  const auto epoch = [] { bleConnectCancelAdvance(); };
  const auto fault = [] { gLifecycleFaulted = true; };
  const auto retirement = [] { gBleClientRetirementPeerMask = 1; };
  const auto recovery = [] { bleCentralRecoveryBlock(true); };
  const auto yield = [] { bleCentralPhoneEnabled(true); };
  endsAtRecheck(kSpyG2, g2Saved, epoch, kStale, false, __LINE__);
  endsAtRecheck(kSpyG2, g2Saved, fault, kFault, false, __LINE__);
  endsAtRecheck(kSpyG2, g2Saved, retirement, kFault, false, __LINE__);
  endsAtRecheck(kSpyG2, g2Saved, recovery, kRecovery, false, __LINE__);
  // The one-shot reseek: only the yield puts it back.
  endsAtRecheck(kSpyRing, ringNudge, ringCancel, kStale, false, __LINE__);
  endsAtRecheck(kSpyRing, ringNudge, [] { gRequestCurrent = false; }, kStale, false, __LINE__);
  endsAtRecheck(kSpyRing, ringNudge, fault, kFault, false, __LINE__);
  endsAtRecheck(kSpyRing, ringNudge, retirement, kFault, false, __LINE__);
  endsAtRecheck(kSpyRing, ringNudge, recovery, kRecovery, false, __LINE__);
  endsAtRecheck(kSpyRing, ringNudge, yield, kYield, true, __LINE__);
  endsAtRecheck(kSpyRing, ringSaved, yield, kYield, false, __LINE__);
  // A manual job's fence no longer current.
  endsAtRecheck(kSpyRing, ringScanConnect, [] { gIntentCurrent = false; }, kStale, false, __LINE__);
  endsAtRecheck(kSpyRing, ringScanConnect, ringCancel, kStale, false, __LINE__);
  endsAtRecheck(kSpyRing, ringScanOnly, ringCancel, kStale, false, __LINE__);
}

// A cancel that lands while the worker waits on a phone turn: the queued jobs
// are dropped once the turn ends, never run.
static void cancelDuringPhoneTurn() {
  using namespace hw1setup;
  gLinks = 4;
  bleCentralPhoneEnabled(true);
  const uint32_t turn = phoneTakeTurn();
  CHECK(g2Saved() && ringMac());
  const size_t g2Runs = runCount(kSpyG2), ringRuns = runCount(kSpyRing);
  bleConnectCancelAdvance();
  ringCancel();
  ADVANCE(500);
  CHECK(hw1setup::grantHeld(gTurn, turn) && gConnectActive() && g2RingConnectInFlight());
  phoneEndTurn(turn);
  CHECK(runCount(kSpyG2) == g2Runs && runCount(kSpyRing) == ringRuns);
  CHECK(lastFinish(kSpyG2).end == End::Dropped && lastFinish(kSpyG2).reason == kStale);
  CHECK(lastFinish(kSpyRing).end == End::Dropped && lastFinish(kSpyRing).reason == kStale);
  CHECK(!gConnectActive() && !g2RingConnectInFlight());
  resetWorld();
}

// A role transition makes jobs wait, neither run nor dropped, until it ends.
static void roleTransitionWaits() {
  hold();
  CHECK(g2Saved() && ringSaved());
  const uint32_t admittedAt = millis();
  gRole = BleRoleTransition::RECOVERING;
  const size_t g2Runs = runCount(kSpyG2), ringRuns = runCount(kSpyRing);
  const size_t g2Fin = finishCount(kSpyG2), ringFin = finishCount(kSpyRing);
  resume();
  ADVANCE(5000);
  CHECK(runCount(kSpyG2) == g2Runs && runCount(kSpyRing) == ringRuns);
  CHECK(finishCount(kSpyG2) == g2Fin && finishCount(kSpyRing) == ringFin);
  CHECK(lastWait(kSpyG2) == hw1setup::kRole && lastWait(kSpyRing) == hw1setup::kRole);
  CHECK(gConnectActive() && g2RingConnectInFlight());
  clearEvents();
  gRole = BleRoleTransition::IDLE;
  ADVANCE(100);
  CHECK((eventsOf({"run G2", "run Ring"}) == std::vector<std::string>{"run G2", "run Ring"}));
  CHECK(lastFinish(kSpyG2).end == hw1setup::End::Ran && lastFinish(kSpyRing).end == hw1setup::End::Ran);
  CHECK(!gConnectActive() && !g2RingConnectInFlight());
  // The ring's in-flight watchdog times its own turn, not its wait in line.
  CHECK(gRingConnectTaskSinceMs >= admittedAt + 5000);
  resetWorld();
}

// Turn order with the real ops: glasses, ring retirement, ring, phone. A
// phone setup in progress is never cancelled by a glasses or ring submit:
// they wait for its end, then run in turn order, and record that wait.
static void turnOrder() {
  // Four links: the ring stays beside the pipe.
  gLinks = 4;
  bleCentralPhoneEnabled(true);
  CHECK(!bleCentralRingShouldYield());
  {
    const uint32_t turn = phoneTakeTurn();
    const uint32_t queuedAt = millis();
    CHECK(ringMac() && g2Saved());  // the ring first, then the glasses
    const size_t g2Runs = runCount(kSpyG2), ringRuns = runCount(kSpyRing);
    const unsigned stops = gScanStops;
    for (int i = 0; i < 10; ++i) ADVANCE(100);
    CHECK(hw1setup::grantHeld(gTurn, turn) && gPhoneReleases + 1 == gPhoneGrants);  // still its turn
    CHECK(runCount(kSpyG2) == g2Runs && runCount(kSpyRing) == ringRuns && gScanStops == stops);
    CHECK(bleSetupAccessoryBusy() && gConnectActive() && g2RingConnectInFlight());
    clearEvents();
    phoneEndTurn(turn);
    CHECK((eventsOf({"run G2", "run Ring", "run Phone"}) == std::vector<std::string>{"run G2", "run Ring"}));
    // Each recorded its wait for the phone's setup: 1000 ms from submission.
    CHECK(millis() - queuedAt == 1000);
    CHECK(lastG2Call().what == "g2 saved" && lastG2Call().wait == 1000);
    CHECK(lastConnect().wait == 1000 && lastSetupWait().label == "ring mac" && lastSetupWait().ms == 1000);
    CHECK(lastRun(kSpyPhone).queuedMs == 0);
  }
  // Glasses running; ring and phone queued behind them: the ring goes first.
  {
    gBlockG2 = true;
    CHECK(g2Saved());
    SETTLE();
    const uint32_t phone = phoneQueue();
    CHECK(ringSaved());
    SETTLE();
    clearEvents();
    gReleaseG2 = true;
    SETTLE();
    CHECK((eventsOf({"run Ring", "run Phone"}) == std::vector<std::string>{"run Ring", "run Phone"}));
    CHECK(gTurn.granted.load() == phone && hw1setup::consumeGrant(gTurn, phone));
    // The phone's turn records nothing on accessory connect records.
    CHECK(gPhoneRunWait.load() == kNoWait && lastSetupWait().label == "phone");
    phoneEndTurn(phone);
    gBlockG2 = false; gReleaseG2 = false;
  }
  resetWorld();
  // Three links: the ring yields and its client is retired, after the
  // glasses and before the phone.
  bleCentralPhoneEnabled(true);
  gRing.client = &gRingClient;
  {
    const uint32_t turn = phoneTakeTurn();
    CHECK(g2Saved());
    g2RingYieldToPhonePipe();
    CHECK(!bleSetupIdle(hw1setup::clientMask(hw1setup::Client::RingRetire)));
    for (int i = 0; i < 5; ++i) ADVANCE(100);
    CHECK(hw1setup::grantHeld(gTurn, turn) && retireCount() == 0);
    // While the glasses run, the phone asks for its next turn.
    gDuringG2Saved = +[] { (void)phoneQueue(); };
    clearEvents();
    phoneEndTurn(turn);
    gDuringG2Saved = nullptr;
    CHECK((eventsOf({"run G2", "run RingRetire", "run Phone"}) ==
           std::vector<std::string>{"run G2", "run RingRetire", "run Phone"}));
    CHECK(retireCount() == 1 && !gRing.client);
    // Ring retirement records no accessory wait; it logs its own.
    const RetireCall retire = LAST(gRetires);
    CHECK(retire.onWorker && retire.wait == kNoWait);
    bool loggedRetire = false;
    for (const auto& w : copyOf(gSetupWaits)) loggedRetire |= w.label == "ring retire";
    CHECK(loggedRetire && lastFinish(kSpyRetire).end == hw1setup::End::Ran);
    const uint32_t next = gTurn.granted.load();
    CHECK(next && hw1setup::consumeGrant(gTurn, next));
    phoneEndTurn(next);
  }
  resetWorld();
}

// The recorded wait leaves out the job's own waits (here G2 audio).
static void recordedWaitExcludesOwnWait() {
  gLinks = 4;
  bleCentralPhoneEnabled(true);
  const uint32_t turn = phoneTakeTurn();
  gAudioCritical = true;
  CHECK(ringMac());
  ADVANCE(300);
  phoneEndTurn(turn);          // the ring job now waits for audio
  CHECK(lastWait(kSpyRing) == hw1setup::kAudio);
  ADVANCE(1900);
  gAudioCritical = false;
  const size_t connects = connectCount();
  ADVANCE(100);
  CHECK(connectCount() == connects + 1);
  const hw1setup::RunInfo run = lastRun(kSpyRing);
  CHECK(run.queuedMs == 2300 && run.selfWaitMs == 2000 && run.audioMs == 2000);
  CHECK(lastConnect().wait == 300 && lastSetupWait().label == "ring mac" && lastSetupWait().ms == 300);
  CHECK(LAST(gAudioEnded) == 2000);
  resetWorld();
}

// The ring's G2-audio wait: logged once per spell, other waits pause but never
// reset its 90 s total, it then expires; a run after it logs the wait.
static void ringAudioWait() {
  using namespace hw1setup;
  gAudioCritical = true;
  const unsigned notes = gAudioWaitNotes;
  const size_t restores = restoreCount(), connects = connectCount(), finishes = finishCount(kSpyRing);
  CHECK(ringNudge());
  SETTLE();
  CHECK(gAudioWaitNotes == notes + 1 && lastWait(kSpyRing) == kAudio);
  ADVANCE(48000);
  for (int i = 0; i < 20; ++i) ADVANCE(100);
  CHECK(gAudioWaitNotes == notes + 1 && finishCount(kSpyRing) == finishes);  // logged once
  // A role spell pauses the audio total (50 s so far) without resetting it.
  gRole = BleRoleTransition::G2_START;
  ADVANCE(100);
  CHECK(lastWait(kSpyRing) == kRole);
  ADVANCE(100000);
  gRole = BleRoleTransition::IDLE;
  ADVANCE(100);
  CHECK(lastWait(kSpyRing) == kAudio && gAudioWaitNotes == notes + 2);
  ADVANCE(39800);
  CHECK(finishCount(kSpyRing) == finishes && g2RingConnectInFlight());
  ADVANCE(100);
  CHECK(finishCount(kSpyRing) == finishes + 1);
  const SpyFinish f = lastFinish(kSpyRing);
  CHECK(f.end == End::Dropped && f.reason == kAudioExpired && f.audioMs == 90000);
  CHECK(LAST(gAudioExpired) == 90000);
  CHECK(connectCount() == connects && restoreCount() == restores && !g2RingConnectInFlight());
  // A connect that waited for audio says so when it starts.
  clearEvents();
  CHECK(ringScanConnect());
  ADVANCE(1900);
  gAudioCritical = false;
  ADVANCE(100);
  CHECK(connectCount() == connects + 1 && LAST(gAudioEnded) == 2000);
  CHECK((eventsOf({"audio wait ended", "ring connect"}) ==
         std::vector<std::string>{"audio wait ended", "ring connect"}));
  // A manual scan never waits for audio.
  gAudioCritical = true;
  const size_t scans = scanCount(), ended = copyOf(gAudioEnded).size();
  const unsigned noted = gAudioWaitNotes;
  CHECK(ringScanOnly());
  SETTLE();
  CHECK(scanCount() == scans + 1 && gAudioWaitNotes == noted && copyOf(gAudioEnded).size() == ended);
  CHECK(lastRun(kSpyRing).selfWaitMs == 0);
  // Glasses jobs never wait for audio either.
  const size_t g2Runs = runCount(kSpyG2);
  CHECK(g2Saved());
  SETTLE();
  CHECK(runCount(kSpyG2) == g2Runs + 1);
  resetWorld();
}

// G2_REPAIR counts as failed unless it ran: dropped, withdrawn or skipped.
static void repairOutcome() {
  using namespace hw1setup;
  gRepair.restart(0, 0);
  gRepair.failed = 2;
  gRepairResult = true;
  CHECK(g2Repair());
  SETTLE();
  CHECK(lastG2Call().what == "g2 repair" && lastG2Call().attempt == 3 && lastG2Call().wait == 0);
  CHECK(repairCount() == 1 && lastRepair().ok && lastRepair().onWorker && !gConnectActive());
  gRepairResult = false;
  CHECK(g2Repair());
  SETTLE();
  CHECK(repairCount() == 2 && !lastRepair().ok);
  gRepairResult = true;
  gRepair.failed = g2repair::kFastTries;  // a 10-minute retry is attempt 0
  CHECK(g2Repair());
  SETTLE();
  CHECK(repairCount() == 3 && lastRepair().ok && lastG2Call().attempt == 0);
  // Dropped, right after one that ran ok.
  endsAtRecheck(kSpyG2, g2Repair, [] { gLifecycleFaulted = true; }, kFault, false, __LINE__);
  CHECK(repairCount() == 4 && !lastRepair().ok && lastRepair().onWorker);
  // Withdrawn while a phone setup holds the worker: on the calling task.
  gLinks = 4;
  bleCentralPhoneEnabled(true);
  const uint32_t turn = phoneTakeTurn();
  const size_t calls = g2CallCount();
  CHECK(g2Repair());
  CHECK(bleSetupWithdraw(BleSetupClient::G2, 0) == BleSetupWithdraw::Withdrawn);
  CHECK(repairCount() == 5 && !lastRepair().ok && !lastRepair().onWorker && !gConnectActive());
  CHECK(lastFinish(kSpyG2).end == End::Withdrawn && !lastFinish(kSpyG2).onWorker);
  phoneEndTurn(turn);
  CHECK(g2CallCount() == calls);
  resetWorld();
  // Skipped: a cancel or fault that lands after ready() said Run.
  gAfterWorkerRoleCheck = +[] { bleConnectCancelAdvance(); };
  CHECK(g2Repair());
  SETTLE();
  CHECK(g2CallCount() == calls && repairCount() == 6 && !lastRepair().ok);
  CHECK(lastFinish(kSpyG2).end == End::Ran && !gConnectActive());
  gAfterWorkerRoleCheck = +[] { gLifecycleFaulted = true; };
  CHECK(g2Repair());
  SETTLE();
  CHECK(g2CallCount() == calls && repairCount() == 7 && !lastRepair().ok);
  gLifecycleFaulted = false;
  // Only repairs report a recovery outcome.
  CHECK(g2Saved());
  SETTLE();
  CHECK(submitG2(BleConnectKind::G2_EYE));
  SETTLE();
  CHECK(repairCount() == 7 && g2CallCount() == calls + 2);
  CHECK(lastG2Call().what == "g2 eye" && lastG2Call().fenced);
  resetWorld();
  // A ring job skips its body on a fault after ready() too.
  const size_t connects = connectCount(), ringRuns = runCount(kSpyRing);
  gAfterWorkerRoleCheck = +[] { gLifecycleFaulted = true; };
  CHECK(ringMac());
  SETTLE();
  CHECK(runCount(kSpyRing) == ringRuns + 1 && connectCount() == connects);
  CHECK(lastFinish(kSpyRing).end == End::Ran && !g2RingConnectInFlight());
  resetWorld();
}

// Submitting: the G2 epoch is bound at submit for epoch 0; a claim or publish
// that fails leaves the slot free and the submitter count balanced.
static void submitEdges() {
  using namespace hw1setup;
  const size_t g2Runs = runCount(kSpyG2);
  CHECK(submitG2(BleConnectKind::G2_SAVED, /*epochZero=*/true));
  SETTLE();
  CHECK(runCount(kSpyG2) == g2Runs + 1 && lastG2Call().epoch == bleConnectCancelEpochSnapshot());
  hold();
  CHECK(submitG2(BleConnectKind::G2_SAVED, /*epochZero=*/true));
  bleConnectCancelAdvance();
  resume();
  CHECK(lastFinish(kSpyG2).end == End::Dropped && lastFinish(kSpyG2).reason == kStale);
  CHECK(runCount(kSpyG2) == g2Runs + 1 && !gConnectActive());
  // A publish that fails takes its claim back.
  gFailPublish = true;
  CHECK(!submitG2(BleConnectKind::G2_SAVED));
  CHECK(bleSetupIdle(kAll) && g2BleConnectSubmitIdle());
  gFailPublish = true;
  CHECK(!g2RingConnect());
  CHECK(bleSetupIdle(kAll) && g2BleConnectSubmitIdle() && !g2RingConnectInFlight());
  gFailPublish = true;
  bleCentralPhoneEnabled(true);
  gRing.client = &gRingClient;
  CHECK(!g2SubmitRingRetire());
  CHECK(bleSetupIdle(kAll) && g2BleConnectSubmitIdle());
  gRing.client = nullptr;
  bleCentralPhoneEnabled(false);
  CHECK(submitG2(BleConnectKind::G2_SAVED) && ringMac());
  SETTLE();
  CHECK(runCount(kSpyG2) == g2Runs + 2 && bleSetupIdle(kAll));
  // A busy slot refuses the claim.
  gLinks = 4;
  bleCentralPhoneEnabled(true);
  const uint32_t turn = phoneTakeTurn();
  CHECK(g2Saved());
  BleConnectJob second{};
  second.kind = BleConnectKind::G2_EYE;
  CHECK(!g2SubmitBleConnect(second) && g2BleConnectSubmitIdle());
  CHECK(bleSetupWithdraw(BleSetupClient::G2, 0) == BleSetupWithdraw::Withdrawn);
  // Teardown (the start block) refuses submissions.
  g2ControlStartBlockAcquire();
  BleConnectJob blocked{};
  blocked.kind = BleConnectKind::G2_SAVED;
  CHECK(!g2SubmitBleConnect(blocked) && !g2SubmitRingRetire());
  g2ControlStartBlockRelease();
  phoneEndTurn(turn);
  resetWorld();
}

// Withdraw: a queued job finishes at once on the caller; a running one is not
// touched. g2Disconnect and g2RingDisconnect take back a queued job at once.
static void withdrawals() {
  using namespace hw1setup;
  gLinks = 4;
  bleCentralPhoneEnabled(true);
  {
    const uint32_t turn = phoneTakeTurn();
    CHECK(ringNudge() && g2Saved());
    const size_t restores = restoreCount();
    CHECK(bleSetupWithdraw(BleSetupClient::Ring, 0) == BleSetupWithdraw::Withdrawn);
    CHECK(lastFinish(kSpyRing).end == End::Withdrawn && !lastFinish(kSpyRing).onWorker);
    CHECK(!g2RingConnectInFlight() && restoreCount() == restores);
    CHECK(bleSetupWithdraw(BleSetupClient::Ring, 0) == BleSetupWithdraw::NotLive);
    // With the glasses also taken back, the accessories are idle while the
    // phone still holds its turn (teardown waits on exactly this).
    CHECK(!bleSetupIdle(kAccessories));
    CHECK(bleSetupWithdraw(BleSetupClient::G2, 0) == BleSetupWithdraw::Withdrawn);
    CHECK(lastFinish(kSpyG2).end == End::Withdrawn && !lastFinish(kSpyG2).onWorker && !gConnectActive());
    CHECK(bleSetupWaitIdle(kAccessories, 6000) && !bleSetupIdle(kAll));
    const size_t g2Runs = runCount(kSpyG2), ringRuns = runCount(kSpyRing);
    phoneEndTurn(turn);
    CHECK(runCount(kSpyG2) == g2Runs && runCount(kSpyRing) == ringRuns);
  }
  // A running job is not withdrawn.
  {
    gBlockRing = true;
    CHECK(ringMac());
    SETTLE();
    const size_t finishes = finishCount(kSpyRing);
    CHECK(bleSetupWithdraw(BleSetupClient::Ring, 0) == BleSetupWithdraw::Running);
    CHECK(finishCount(kSpyRing) == finishes && g2RingConnectInFlight());
    CHECK(!bleSetupWaitIdle(kAccessories, 300));
    gReleaseRing = true;
    SETTLE();
    CHECK(finishCount(kSpyRing) == finishes + 1 && lastFinish(kSpyRing).end == End::Ran);
    CHECK(lastFinish(kSpyRing).onWorker && !g2RingConnectInFlight());
    gBlockRing = false; gReleaseRing = false;
  }
  // g2RingDisconnect: the queued job is taken back after the logical down.
  {
    const uint32_t turn = phoneTakeTurn();
    CHECK(ringNudge());
    const size_t restores = restoreCount(), finishes = finishCount(kSpyRing);
    clearEvents();
    g2RingDisconnect(true);
    CHECK(finishCount(kSpyRing) == finishes + 1 && lastFinish(kSpyRing).end == End::Withdrawn);
    CHECK(!lastFinish(kSpyRing).onWorker && !g2RingConnectInFlight() && restoreCount() == restores);
    CHECK(eventAt("ring down (user)") < eventAt("finish Ring Withdrawn"));
    const size_t ringRuns = runCount(kSpyRing);
    phoneEndTurn(turn);
    CHECK(runCount(kSpyRing) == ringRuns);
  }
  // g2RingDisconnect with a manual scan running: it stops the scan.
  {
    gBlockRing = true;
    CHECK(ringScanOnly());
    SETTLE();
    const unsigned stops = gScanStops;
    g2RingDisconnect(false);
    SETTLE();
    CHECK(gScanStops == stops + 1 && lastFinish(kSpyRing).end == End::Ran && lastFinish(kSpyRing).onWorker);
    CHECK(!g2RingConnectInFlight());
    gBlockRing = false; gScanStopRequested = false;
  }
  // g2Disconnect: a queued glasses job is taken back before the in-flight
  // cancel, which then has nothing to cancel.
  {
    const uint32_t turn = phoneTakeTurn();
    CHECK(g2Saved());
    const unsigned reads = gG2StateReads, delays = gTestDelays, left = gLeftClient.disconnects;
    const size_t finishes = finishCount(kSpyG2), g2Runs = runCount(kSpyG2);
    CHECK(g2Disconnect(false));
    CHECK(finishCount(kSpyG2) == finishes + 1 && lastFinish(kSpyG2).end == End::Withdrawn);
    CHECK(!lastFinish(kSpyG2).onWorker && !gConnectActive());
    CHECK(gG2StateReads == reads && gTestDelays == delays && gLeftClient.disconnects == left);
    phoneEndTurn(turn);
    CHECK(runCount(kSpyG2) == g2Runs);
  }
  // g2Disconnect with a glasses setup running: not withdrawn; the epoch ends
  // it, and g2Disconnect waits for its finish.
  {
    gBlockG2 = true;
    CHECK(g2Saved());
    SETTLE();
    const unsigned reads = gG2StateReads, left = gLeftClient.disconnects;
    const size_t finishes = finishCount(kSpyG2);
    CHECK(g2Disconnect(false));
    CHECK(gG2StateReads == reads + 1 && gLeftClient.disconnects == left + 1);
    CHECK(finishCount(kSpyG2) == finishes + 1 && lastFinish(kSpyG2).end == End::Ran);
    CHECK(lastFinish(kSpyG2).onWorker && !gConnectActive());
    gBlockG2 = false;
  }
  resetWorld();
}

// A glasses submit takes the radio from a ring scan in progress (and only from
// a scan); g2CancelActiveRingScan stops only ring scans.
static void preemption() {
  struct Case { bool (*submit)(); bool scanning; };
  const Case cases[] = {{ringScanOnly, true}, {ringScanConnect, true}, {ringMac, false}, {ringSaved, false}};
  for (const Case& c : cases) {
    gBlockRing = true; gReleaseRing = false; gScanStopRequested = false;
    CHECK(c.submit());
    SETTLE();
    const unsigned stops = gScanStops;
    clearEvents();
    CHECK(g2Saved());
    if (c.scanning) {
      SETTLE();
      CHECK(gScanStops == stops + 1);
    } else {
      SETTLE();
      CHECK(gScanStops == stops);
      g2CancelActiveRingScan();
      CHECK(gScanStops == stops);
      gReleaseRing = true;
      SETTLE();
    }
    CHECK((eventsOf({"finish Ring Ran", "run G2"}) == std::vector<std::string>{"finish Ring Ran", "run G2"}));
    CHECK(!g2RingConnectInFlight() && !gConnectActive());
  }
  // The manual scan saw the glasses job waiting (it ends early for one).
  CHECK(copyOf(gScans).front().endWhenOtherQueued);
  bool sawQueued = false;
  for (const auto& scan : copyOf(gScans)) sawQueued |= scan.otherQueuedAtEnd;
  CHECK(sawQueued);
  // g2CancelActiveRingScan: a running manual scan is stopped; glasses and an
  // idle worker are left alone.
  gBlockRing = true; gReleaseRing = false; gScanStopRequested = false;
  CHECK(ringScanOnly());
  SETTLE();
  unsigned stops = gScanStops;
  g2CancelActiveRingScan();
  SETTLE();
  CHECK(gScanStops == stops + 1 && !g2RingConnectInFlight());
  gBlockRing = false;
  gBlockG2 = true; gReleaseG2 = false;
  CHECK(g2Saved());
  SETTLE();
  stops = gScanStops;
  g2CancelActiveRingScan();
  CHECK(gScanStops == stops);
  gReleaseG2 = true;
  SETTLE();
  g2CancelActiveRingScan();
  CHECK(gScanStops == stops);
  resetWorld();
}

// Ring retirement for the phone pipe: submitted only when the ring must yield
// and a ring client exists; one at a time; dropped (never waiting) when it no
// longer applies; its run retires the client.
static void ringRetirement() {
  using namespace hw1setup;
  // No submit without a client, with four links, or with the pipe off.
  const uint32_t before = probeToken();
  const unsigned stops = gScanStops, readies = gSpy[kSpyRetire].readies;
  const size_t retired = retireCount();
  bleCentralPhoneEnabled(true);
  for (int i = 0; i < 10; ++i) g2RingYieldToPhonePipe();  // no client
  gRing.client = &gRingClient;
  gLinks = 4;
  for (int i = 0; i < 10; ++i) g2RingYieldToPhonePipe();
  gLinks = 3;
  bleCentralPhoneEnabled(false);
  for (int i = 0; i < 10; ++i) g2RingYieldToPhonePipe();
  SETTLE();
  CHECK(probeToken() == before + 1 && gSpy[kSpyRetire].readies == readies && retireCount() == retired);
  CHECK(gScanStops == stops);
  // Yielding with a client: one job; a second submit is refused while queued.
  bleCentralPhoneEnabled(true);
  hold();
  const uint32_t first = probeToken();
  g2RingYieldToPhonePipe();
  g2RingYieldToPhonePipe();
  CHECK(!g2SubmitRingRetire());
  CHECK(!bleSetupIdle(clientMask(Client::RingRetire)));
  resume();
  CHECK(probeToken() == first + 2);  // one claim in between
  CHECK(retireCount() == retired + 1 && !gRing.client && lastFinish(kSpyRetire).end == End::Ran);
  CHECK(bleSetupIdle(kAll));
  // Queued, then the pipe stops: dropped without retiring.
  gRing.client = &gRingClient;
  hold();
  g2RingYieldToPhonePipe();
  bleCentralPhoneEnabled(false);
  const size_t runs = runCount(kSpyRetire);
  resume();
  CHECK(lastFinish(kSpyRetire).end == End::Dropped && lastFinish(kSpyRetire).reason == kStale);
  CHECK(runCount(kSpyRetire) == runs && retireCount() == retired + 1 && gRing.client == &gRingClient);
  // Queued, then a role transition: dropped at once, not left waiting; the
  // main loop asks again on its next pass.
  bleCentralPhoneEnabled(true);
  hold();
  g2RingYieldToPhonePipe();
  gRole = BleRoleTransition::G2_STOP;
  resume();
  CHECK(lastFinish(kSpyRetire).end == End::Dropped && lastFinish(kSpyRetire).reason == kRole);
  CHECK(waitsOf(kSpyRetire).empty() && bleSetupIdle(kAll) && runCount(kSpyRetire) == runs);
  gRole = BleRoleTransition::IDLE;
  // Faults and the recovery block drop it too.
  hold();
  g2RingYieldToPhonePipe();
  bleCentralRecoveryBlock(true);
  resume();
  CHECK(lastFinish(kSpyRetire).reason == kRecovery && runCount(kSpyRetire) == runs);
  bleCentralRecoveryBlock(false);
  g2RingYieldToPhonePipe();
  SETTLE();
  CHECK(retireCount() == retired + 2 && !gRing.client);
  resetWorld();
}

// bleSetupWorkerRelease: once asked, the worker leaves on its own when idle,
// and a start brings a new task on the wake semaphore made once. A job in
// progress or waiting its turn is never abandoned: the release waits for it,
// or gives up at its timeout and leaves the worker as it was.
static void workerRelease() {
  using namespace hw1setup;
  // Never from the worker itself: refused, and the worker stays ready.
  gDuringG2Saved = +[] { CHECK(!bleSetupWorkerRelease(0) && bleSetupWorkerStart()); };
  const size_t g2Runs = runCount(kSpyG2);
  CHECK(g2Saved());
  SETTLE();
  gDuringG2Saved = nullptr;
  CHECK(runCount(kSpyG2) == g2Runs + 1 && lastFinish(kSpyG2).end == End::Ran && bleSetupIdle(kAll));
  // A ring job in progress: while the release waits, claims are refused and a
  // start says no; the job finishes first, then the worker leaves.
  gBlockRing = true;
  CHECK(ringMac());
  SETTLE();
  gOnTestDelay = +[] {
    uint32_t token = 0;
    CHECK(!bleSetupClaim(BleSetupClient::Phone, &token) && !bleSetupWorkerStart());
    CHECK(!bleSetupIdle(kAll) && g2RingConnectInFlight());
    gReleaseRing = true;
  };
  const size_t finishes = finishCount(kSpyRing);
  CHECK(bleSetupWorkerRelease(2000));
  CHECK(!gOnTestDelay.load());
  CHECK(finishCount(kSpyRing) == finishes + 1 && lastFinish(kSpyRing).end == End::Ran && lastFinish(kSpyRing).onWorker);
  CHECK(!g2RingConnectInFlight());
  gWorker.join();
  gBlockRing = false; gReleaseRing = false;
  // No worker: a claim is refused, a release has nothing to do, and a start
  // creates a new task without making the semaphore again.
  uint32_t token = 0;
  CHECK(!bleSetupClaim(BleSetupClient::Phone, &token));
  CHECK(bleSetupWorkerRelease(1000) && !gWorker.joinable());
  CHECK(bleSetupWorkerStart() && bleSetupWorkerStart());
  SETTLE();
  CHECK(gWorker.joinable() && gWakeCreates == 1);
  CHECK(gWorkerName == "ble_setup" && gWorkerStack == 5120 && gWorkerPriority == 5);
  CHECK(ringMac());  // the new worker serves
  SETTLE();
  CHECK(finishCount(kSpyRing) == finishes + 2 && lastFinish(kSpyRing).end == End::Ran && lastFinish(kSpyRing).onWorker);
  // Timeout: a job in progress outlasts the wait. The worker is kept, claims
  // open again, and the job ends as it would have.
  gBlockRing = true;
  CHECK(ringMac());
  SETTLE();
  const unsigned delays = gTestDelays;
  const uint32_t before = millis();
  CHECK(!bleSetupWorkerRelease(300));
  CHECK(millis() - before == 300 && gTestDelays == delays + 60);
  CHECK(g2RingConnectInFlight() && !bleSetupIdle(kAll) && bleSetupWorkerStart());
  gReleaseRing = true;
  SETTLE();
  CHECK(finishCount(kSpyRing) == finishes + 3 && lastFinish(kSpyRing).end == End::Ran && lastFinish(kSpyRing).onWorker);
  CHECK(bleSetupIdle(kAll) && gWorker.joinable() && probeToken() != 0);
  gBlockRing = false; gReleaseRing = false;
  // A queued job waiting its turn (a role transition) is not abandoned
  // either: the worker rechecks it every 100 ms and stays; it runs once the
  // transition ends.
  hold();
  CHECK(ringSaved());
  gRole = BleRoleTransition::RECOVERING;
  const size_t ringRuns = runCount(kSpyRing);
  resume();
  CHECK(runCount(kSpyRing) == ringRuns && lastWait(kSpyRing) == kRole && g2RingConnectInFlight());
  CHECK(!bleSetupWorkerRelease(300));
  CHECK(runCount(kSpyRing) == ringRuns && g2RingConnectInFlight() && bleSetupWorkerStart());
  gRole = BleRoleTransition::IDLE;
  ADVANCE(100);
  CHECK(runCount(kSpyRing) == ringRuns + 1 && lastFinish(kSpyRing).end == End::Ran && !g2RingConnectInFlight());
  // Idle: the release wakes the parked worker, which leaves at once.
  CHECK(bleSetupIdle(kAll) && bleSetupWorkerRelease(1000));
  gWorker.join();
  CHECK(bleSetupWorkerStart());
  SETTLE();
  CHECK(gWakeCreates == 1);
  resetWorld();
}

int main() {
  std::atomic<bool> done{false};
  std::thread([&done] {
    for (int i = 0; i < 600 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!done.load()) die("watchdog: the suite took over 60 s", __LINE__);
  }).detach();

  installSpy<kSpyG2>(g2SetupOps);
  installSpy<kSpyRetire>(g2RingRetireSetupOps);
  installSpy<kSpyRing>(g2RingSetupOps);
  installSpy<kSpyPhone>(phonePipeSetupOps);
  uint32_t early = 0;
  CHECK(!bleSetupClaim(BleSetupClient::G2, &early));  // not before the worker runs
  CHECK(bleSetupWorkerStart() && bleSetupWorkerStart());
  SETTLE();
  CHECK(gWorkerName == "ble_setup" && gWorkerStack == 5120 && gWorkerPriority == 5 && gWorkerCore == APP_CORE);

  requestRoundTrip();
  pipeOffRingPathsRun();
  refusalsAndFourLinks();
  queuedRingJobsYield();
  cancelsEndQueuedJobs();
  cancelDuringPhoneTurn();
  roleTransitionWaits();
  turnOrder();
  recordedWaitExcludesOwnWait();
  ringAudioWait();
  repairOutcome();
  submitEdges();
  withdrawals();
  preemption();
  ringRetirement();
  workerRelease();

  {
    std::lock_guard<std::mutex> lock(gSched.m);
    gSched.shutdown = true;
    gSched.cv.notify_all();
  }
  gWorker.join();
  done = true;
  std::printf("Ring yield queue: %ld checks. Real worker, ops and submitters: refusals while yielding and"
              " four links; turn order G2 > retire > ring > phone, a phone turn never cancelled; cancels,"
              " faults, recovery, yield end a queued job at the next recheck with one finish and the flag"
              " released; role waits; rule R; repair outcomes; G2 audio wait; recorded wait; withdraw;"
              " preemption; ring retirement; worker release\n", gChecks.load());
  return 0;
}
