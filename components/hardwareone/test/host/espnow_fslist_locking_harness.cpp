// Host harness for the FS-RPC lock discipline of System_ESPNow_FsList.cpp.
//
// test_espnow_fslist_locking.py splices in the production pending table,
// deferred slot, lock guard, send/cancel API, request capture, reply matching,
// cmd_exec runner and timeout sweep. Host threads stand in for httpd (client
// sends), espnow_task (RX capture and replies) and cmd_exec (the deferred
// runner). The FreeRTOS mutex double honors bounded waits and records the
// owning thread, so the checks can tell whether a send or a worker ran with
// the module mutex held. The per-op workers (processListDeferred ...) are
// counting doubles; the tx dispatcher and radio are not modeled.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

static unsigned checks = 0;
static unsigned failures = 0;
static void check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

// ---- Arduino / logging ------------------------------------------------------
static std::atomic<uint32_t> nowMs{10000};
static uint32_t millis() { return nowMs.load(); }
// strlcpy comes from the host libc.
static void logStub(const char*, ...) {}
#define DEBUG_ESPNOWF(...) logStub(__VA_ARGS__)
#define WARN_ESPNOWF(...)  logStub(__VA_ARGS__)
#define ERROR_ESPNOWF(...) logStub(__VA_ARGS__)
#define EXT_RAM_BSS_ATTR

// ---- FreeRTOS mutex double ----------------------------------------------------
using TickType_t = uint32_t;
static constexpr TickType_t portMAX_DELAY = 0xffffffffu;
static constexpr int pdTRUE = 1;
static constexpr int pdFALSE = 0;
static TickType_t pdMS_TO_TICKS(TickType_t ms) { return ms; }
struct HostMutex {
  std::timed_mutex m;
  std::atomic<std::thread::id> owner{};
};
using SemaphoreHandle_t = HostMutex*;
static HostMutex gMutexStorage;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return &gMutexStorage; }
static int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
  bool ok;
  if (wait == portMAX_DELAY) {
    s->m.lock();
    ok = true;
  } else {
    ok = s->m.try_lock_for(std::chrono::milliseconds(wait));
  }
  if (ok) s->owner.store(std::this_thread::get_id());
  return ok ? pdTRUE : pdFALSE;
}
static void xSemaphoreGive(SemaphoreHandle_t s) {
  if (s->owner.load() != std::this_thread::get_id()) {
    ++failures;
    std::fprintf(stderr, "FAIL: mutex given back by a thread that does not hold it\n");
  }
  s->owner.store(std::thread::id());
  s->m.unlock();
}

// ---- wire ---------------------------------------------------------------------
constexpr unsigned ESPNOW_V4_MAX_PLAINTEXT = 202;
enum : uint8_t {
  ESPNOW_V4_TYPE_FS_LIST_REQ = 70, ESPNOW_V4_TYPE_FS_LIST_REPLY = 71,
  ESPNOW_V4_TYPE_FS_STAT_REQ = 72, ESPNOW_V4_TYPE_FS_STAT_REPLY = 73,
  ESPNOW_V4_TYPE_FS_GET_REQ = 74,  ESPNOW_V4_TYPE_FS_GET_ACK = 75,
};
// INSERT_WIRE
// INSERT_HEADER

// ---- transport / cmd_exec doubles (declared here, defined after production) ---
static uint32_t generateMessageId() { return 7; }
namespace espnowtx {
bool sendAead(const uint8_t* mac, uint8_t type, uint16_t flags, uint32_t msgId,
              const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);
bool sendAeadSync(const uint8_t* mac, uint8_t type, uint16_t flags, uint32_t msgId,
                  const uint8_t* payload, uint16_t payloadLen, uint8_t ttl, uint32_t timeoutMs);
}
static bool submitDeferredToCmdExec(void (*fn)(void*), void* arg);

// INSERT_PRODUCTION

// ---- doubles that need the production statics -----------------------------------
static bool heldByMe() { return sMutex && sMutex->owner.load() == std::this_thread::get_id(); }

struct Sent {
  uint8_t type;
  std::vector<uint8_t> payload;
  bool sync;
};
static std::mutex sentMutex;
static std::vector<Sent> sent;
static std::atomic<bool> syncSendShouldBlock{false};
static std::atomic<bool> syncSendResult{true};
static std::atomic<unsigned> syncSendsInFlight{0};
static std::atomic<unsigned> sendsWhileLocked{0};
static std::mutex gateMutex;
static std::condition_variable gateCv;
static bool gateOpen = true;

namespace espnowtx {
bool sendAead(const uint8_t*, uint8_t type, uint16_t, uint32_t, const uint8_t* p, uint16_t n, uint8_t) {
  std::lock_guard<std::mutex> g(sentMutex);
  sent.push_back({type, std::vector<uint8_t>(p, p + n), false});
  return true;
}
// Stands in for the blocking tx-queue submit: optionally parks on a gate (a
// stalled dispatcher) and counts calls made while the caller owns sMutex.
bool sendAeadSync(const uint8_t*, uint8_t type, uint16_t, uint32_t, const uint8_t* p, uint16_t n,
                  uint8_t, uint32_t) {
  if (heldByMe()) ++sendsWhileLocked;
  ++syncSendsInFlight;
  if (syncSendShouldBlock) {
    std::unique_lock<std::mutex> g(gateMutex);
    gateCv.wait(g, [] { return gateOpen; });
  }
  --syncSendsInFlight;
  std::lock_guard<std::mutex> g(sentMutex);
  sent.push_back({type, std::vector<uint8_t>(p, p + n), true});
  return syncSendResult;
}
}  // namespace espnowtx

static std::mutex cmdExecMutex;
static std::vector<void (*)(void*)> cmdExecQueue;
static bool cmdExecAccepts = true;
static bool submitDeferredToCmdExec(void (*fn)(void*), void* arg) {
  (void)arg;
  std::lock_guard<std::mutex> g(cmdExecMutex);
  if (!cmdExecAccepts) return false;
  cmdExecQueue.push_back(fn);
  return true;
}
static size_t cmdExecQueued() {
  std::lock_guard<std::mutex> g(cmdExecMutex);
  return cmdExecQueue.size();
}
// Plays cmd_exec: runs every queued job on the calling thread.
static void runCmdExecQueue() {
  std::vector<void (*)(void*)> q;
  {
    std::lock_guard<std::mutex> g(cmdExecMutex);
    q.swap(cmdExecQueue);
  }
  for (auto fn : q) fn(nullptr);
}

static std::atomic<unsigned> listWork{0}, statWork{0}, getWork{0}, workWhileLocked{0};
static void processListDeferred(const uint8_t*, const V4PayloadFsListReq&) {
  if (heldByMe()) ++workWhileLocked;
  ++listWork;
}
static void processStatDeferred(const uint8_t*, const V4PayloadFsStatReq&) {
  if (heldByMe()) ++workWhileLocked;
  ++statWork;
}
static void processGetDeferred(const uint8_t*, const V4PayloadFsGetReq&) {
  if (heldByMe()) ++workWhileLocked;
  ++getWork;
}

// ---- helpers --------------------------------------------------------------------
static const uint8_t peerA[6] = {0x02, 0, 0, 0, 0, 0x01};
static const uint8_t peerB[6] = {0x02, 0, 0, 0, 0, 0x02};
static std::atomic<unsigned> listCallbacks{0}, statCallbacks{0}, getCallbacks{0};
static void onList(const uint8_t*, const V4PayloadFsListReplyHeader*, const V4PayloadFsEntry*) { ++listCallbacks; }
static void onStat(const uint8_t*, const V4PayloadFsStatReply*) { ++statCallbacks; }
static void onGet(const uint8_t*, const V4PayloadFsGetAck*) { ++getCallbacks; }

static void gate(bool open) {
  {
    std::lock_guard<std::mutex> g(gateMutex);
    gateOpen = open;
  }
  gateCv.notify_all();
}

static bool mutexFree() {
  if (!gMutexStorage.m.try_lock()) return false;
  gMutexStorage.m.unlock();
  return true;
}

static void resetModule() {
  check(mutexFree(), "module mutex free between cases");
  std::memset(sPending, 0, sizeof(sPending));
  std::memset(&sDeferred, 0, sizeof(sDeferred));
  sNextReqId = 1;
  {
    std::lock_guard<std::mutex> g(sentMutex);
    sent.clear();
  }
  {
    std::lock_guard<std::mutex> g(cmdExecMutex);
    cmdExecQueue.clear();
    cmdExecAccepts = true;
  }
  syncSendShouldBlock = false;
  syncSendResult = true;
  listWork = 0; statWork = 0; getWork = 0; workWhileLocked = 0; sendsWhileLocked = 0;
  listCallbacks = 0; statCallbacks = 0; getCallbacks = 0;
  gate(true);
}

static V4PayloadFsListReq listReq(uint32_t reqId, const char* path) {
  V4PayloadFsListReq r = {};
  r.reqId = reqId;
  r.maxEntries = FS_LIST_ENTRIES_PER_REPLY;
  strlcpy(r.path, path, sizeof(r.path));
  return r;
}

static void deliverListRequest(const uint8_t mac[6], uint32_t reqId) {
  V4PayloadFsListReq r = listReq(reqId, "/");
  fsListOnRequestReceived(mac, reinterpret_cast<const uint8_t*>(&r), sizeof(r));
}

// TOO_BUSY replies sent fire-and-forget (the RX-safe path) for any op.
static unsigned busyReplies() {
  std::lock_guard<std::mutex> g(sentMutex);
  unsigned n = 0;
  for (const auto& s : sent) {
    if (s.sync || s.payload.size() < 5) continue;
    if (s.type != ESPNOW_V4_TYPE_FS_LIST_REPLY && s.type != ESPNOW_V4_TYPE_FS_STAT_REPLY &&
        s.type != ESPNOW_V4_TYPE_FS_GET_ACK) continue;
    if (s.payload[4] == FS_LIST_STATUS_TOO_BUSY) ++n;  // status follows the 4-byte reqId in all three
  }
  return n;
}

static unsigned liveSlots() {
  unsigned n = 0;
  for (const auto& p : sPending) n += p.reqId != 0;
  return n;
}

static void waitFor(const std::atomic<unsigned>& v, unsigned target) {
  for (int i = 0; i < 2000 && v.load() != target; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// Another task holding the module mutex for `ms` (longer than the 50 ms bounded wait).
struct ForeignHold {
  std::atomic<bool> holding{false};
  std::thread t;
  explicit ForeignHold(unsigned ms) {
    t = std::thread([this, ms] {
      xSemaphoreTake(sMutex, portMAX_DELAY);
      holding = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      holding = false;
      xSemaphoreGive(sMutex);
    });
    for (int i = 0; i < 2000 && !holding.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  void join() { t.join(); }
};

int main() {
  fsListInit();
  check(sMutex != nullptr, "fsListInit creates the module mutex");

  // Case 1: two devices browse each other. A captured LIST from B is queued for
  // cmd_exec while a local client send to B is blocked in the tx queue.
  {
    resetModule();
    deliverListRequest(peerB, 0x1001);
    check(sDeferred.pending && cmdExecQueued() == 1, "case1: request captured and its job queued");
    syncSendShouldBlock = true;
    gate(false);
    std::atomic<uint32_t> clientId{0};
    std::thread client([&] { clientId = fsListSendRequest(peerB, "/", 0, onList); });
    waitFor(syncSendsInFlight, 1);
    std::thread cmdExec([] { runCmdExecQueue(); });
    cmdExec.join();
    gate(true);
    client.join();
    check(clientId.load() != 0, "case1: client send succeeds");
    check(listWork == 1, "case1: captured LIST served by the cmd_exec job while a client send is blocked");
    check(!sDeferred.pending, "case1: deferred slot released after the cmd_exec job (no wedge)");
    check(sendsWhileLocked == 0, "case1: fsListSendRequest sends without holding the module mutex");
    deliverListRequest(peerB, 0x1002);
    check(busyReplies() == 0 && sDeferred.pending, "case1: next request captured, not answered TOO_BUSY");
    runCmdExecQueue();
    check(listWork == 2 && !sDeferred.pending, "case1: next request served");
    check(workWhileLocked == 0, "case1: workers run without the module mutex");
  }

  // Case 2: RX capture and reply delivery proceed while a client send blocks.
  {
    resetModule();
    syncSendShouldBlock = true;
    gate(false);
    std::atomic<uint32_t> clientId{0};
    std::thread client([&] { clientId = fsListSendRequest(peerA, "/x", 0, onList); });
    waitFor(syncSendsInFlight, 1);
    deliverListRequest(peerB, 0x2001);
    check(sDeferred.pending && cmdExecQueued() == 1, "case2: request captured during a blocked client send");
    gate(true);
    client.join();
    check(clientId.load() != 0, "case2: client send succeeds");
    runCmdExecQueue();
    check(listWork == 1 && !sDeferred.pending, "case2: request captured during the send is served");
    V4PayloadFsListReplyHeader reply = {};
    reply.reqId = clientId.load();
    reply.status = FS_LIST_STATUS_OK;
    fsListOnReplyReceived(peerA, reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));
    check(listCallbacks == 1 && liveSlots() == 0, "case2: client reply reaches its callback once");
  }

  // Case 3: the cmd_exec job outlasts a foreign hold longer than the 50 ms guard.
  {
    resetModule();
    deliverListRequest(peerB, 0x3001);
    ForeignHold hold(120);
    runCmdExecQueue();
    hold.join();
    check(listWork == 1, "case3: cmd_exec job waits out a 120 ms hold and serves the request");
    check(!sDeferred.pending, "case3: deferred slot released after a 120 ms hold");
    deliverListRequest(peerB, 0x3002);
    check(busyReplies() == 0 && sDeferred.pending, "case3: next request captured, not answered TOO_BUSY");
    runCmdExecQueue();
  }

  // Case 4: a capture that cannot take the lock in espnow_task's bounded wait answers TOO_BUSY.
  {
    resetModule();
    ForeignHold hold(120);
    deliverListRequest(peerB, 0x4001);
    hold.join();
    check(busyReplies() == 1, "case4: request arriving during a 120 ms hold is answered TOO_BUSY, not dropped");
    check(!sDeferred.pending && cmdExecQueued() == 0, "case4: nothing captured on a lock miss");
  }

  // Case 5 (preserved): send failure returns 0, frees the slot, fires no callback later.
  {
    resetModule();
    syncSendResult = false;
    check(fsListSendRequest(peerA, "/", 0, onList) == 0, "case5: failed send returns 0");
    check(fsStatSendRequest(peerA, "/", onStat) == 0, "case5: failed STAT send returns 0");
    check(fsGetSendRequest(peerA, "/f.txt", onGet) == 0, "case5: failed GET send returns 0");
    check(liveSlots() == 0, "case5: failed sends leave no pending slot");
    nowMs += FS_LIST_REQUEST_TIMEOUT_MS + 1;
    fsListTick();
    check(listCallbacks == 0 && statCallbacks == 0 && getCallbacks == 0,
          "case5: no callback for a request that never left");
  }

  // Case 6 (preserved): cmd_exec queue full answers TOO_BUSY and frees the slot.
  {
    resetModule();
    {
      std::lock_guard<std::mutex> g(cmdExecMutex);
      cmdExecAccepts = false;
    }
    deliverListRequest(peerB, 0x6001);
    check(busyReplies() == 1 && !sDeferred.pending, "case6: cmd_exec queue full -> TOO_BUSY, slot free");
  }

  // Case 7 (preserved): a second request while one is pending gets TOO_BUSY.
  {
    resetModule();
    deliverListRequest(peerB, 0x7001);
    deliverListRequest(peerA, 0x7002);
    check(busyReplies() == 1 && cmdExecQueued() == 1, "case7: single-slot serialization preserved");
    runCmdExecQueue();
    check(listWork == 1 && !sDeferred.pending, "case7: first request served");
  }

  // Case 8: STAT and GET senders also send off-lock; replies reach the typed callback.
  {
    resetModule();
    uint32_t statId = fsStatSendRequest(peerA, "/", onStat);
    uint32_t getId = fsGetSendRequest(peerA, "/f.txt", onGet);
    check(statId != 0 && getId != 0 && statId != getId, "case8: STAT and GET sends succeed with distinct reqIds");
    check(sendsWhileLocked == 0, "case8: fsStatSendRequest/fsGetSendRequest send without holding the module mutex");
    check(liveSlots() == 2, "case8: one pending slot per request");
    V4PayloadFsStatReply stat = {};
    stat.reqId = statId;
    fsStatOnReplyReceived(peerA, reinterpret_cast<const uint8_t*>(&stat), sizeof(stat));
    V4PayloadFsGetAck ack = {};
    ack.reqId = getId;
    fsGetOnAckReceived(peerA, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    check(statCallbacks == 1 && getCallbacks == 1 && liveSlots() == 0,
          "case8: STAT reply and GET ack reach their callbacks once");
  }

  // Case 9: cancel during a foreign hold still frees the slot, so the timeout
  // sweep never fires the cancelled request's callback.
  {
    resetModule();
    uint32_t id = fsListSendRequest(peerA, "/", 0, onList);
    check(id != 0, "case9: send succeeds");
    {
      ForeignHold hold(120);
      fsListCancel(id);
      hold.join();
    }
    check(liveSlots() == 0, "case9: fsListCancel frees the slot even when the lock is briefly held elsewhere");
    nowMs += FS_LIST_REQUEST_TIMEOUT_MS + 1;
    fsListTick();
    check(listCallbacks == 0, "case9: no callback after fsListCancel returned");
  }

  check(mutexFree(), "module mutex free at exit");
  std::printf("espnow_fslist_locking: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
