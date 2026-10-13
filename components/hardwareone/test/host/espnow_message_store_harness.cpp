// Host harness for the ESP-NOW peer message store (System_ESPNow.cpp, section
// "Per-Device Message Buffer Management"). test_espnow_message_store.py inserts
// the production types, declarations, EspNowState members and the whole store
// section; this file supplies the FreeRTOS mutex, allocation, clock and logging
// doubles and the cases. Each case runs in its own process (argv[1]) so a
// sanitizer abort in one case cannot hide the others.
//
// HW1_HISTORY_COPY_API is 1 when the header declares espnowCopyCollapsedHead.
// At 0 (a tree without the copy accessor) the harness reads a ref the way the
// OLED and G2 screens did there: through the ref's live-ring pointer.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#ifndef HW1_HISTORY_COPY_API
#define HW1_HISTORY_COPY_API 1
#endif

// ---- Build configuration the store sees on a PSRAM build ---------------------
#define MESSAGES_PER_DEVICE 250
// INSERT_DEFINES

// ---- FreeRTOS mutex doubles ----------------------------------------------------
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

struct StaticSemaphore_t {
  std::timed_mutex mutex;
};
typedef StaticSemaphore_t* SemaphoreHandle_t;

static std::atomic<int> gLockCreates{0};
static std::atomic<int> gLockTakes{0};
static std::atomic<int> gLockGives{0};
static std::atomic<int> gNestedTakes{0};
static std::atomic<bool> gLockRefuse{false};
static std::atomic<std::thread::id> gLockOwner{std::thread::id()};

static bool lockHeldByMe() { return gLockOwner.load() == std::this_thread::get_id(); }

inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  gLockCreates++;
  return storage;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
  if (!handle) return pdFALSE;
  if (lockHeldByMe()) {  // a FreeRTOS mutex is not recursive: this would deadlock
    gNestedTakes++;
    return pdFALSE;
  }
  if (gLockRefuse.load()) return pdFALSE;
  if (!handle->mutex.try_lock_for(std::chrono::milliseconds(ticks))) return pdFALSE;
  gLockOwner.store(std::this_thread::get_id());
  gLockTakes++;
  return pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
  gLockOwner.store(std::thread::id());
  gLockGives++;
  handle->mutex.unlock();
  return pdTRUE;
}

// ---- Allocation, clock and logging doubles ----------------------------------
enum class AllocPref { PreferPSRAM, PreferInternal };

static std::atomic<int> gAllocCalls{0};
static std::atomic<bool> gAllocFail{false};
static std::mutex gAllocTagsMutex;
static std::map<std::string, int> gAllocTags;

inline void* ps_alloc(size_t size, AllocPref, const char* tag) {
  gAllocCalls++;
  {
    std::lock_guard<std::mutex> lock(gAllocTagsMutex);
    gAllocTags[tag ? tag : "(null)"]++;
  }
  if (gAllocFail.load()) return nullptr;
  return std::malloc(size);
}

static std::atomic<unsigned long> gMillis{1000};
inline unsigned long millis() { return gMillis.fetch_add(1) + 1; }

static std::atomic<int> gLogsUnderLock{0};
static std::atomic<int> gDebugLogs{0};
static std::atomic<int> gErrorLogs{0};
static std::atomic<int> gBroadcasts{0};
static std::mutex gLogMutex;
static std::string gLastBroadcast;
static std::string gLastError;

static void recordLine(std::atomic<int>& counter, std::string* last, const char* fmt, va_list args) {
  if (lockHeldByMe()) gLogsUnderLock++;  // the store lock must stay a leaf
  counter++;
  char line[256];
  std::vsnprintf(line, sizeof(line), fmt, args);
  if (last) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    *last = line;
  }
}

__attribute__((format(printf, 1, 2))) inline void debugLogDouble(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  recordLine(gDebugLogs, nullptr, fmt, args);
  va_end(args);
}
__attribute__((format(printf, 1, 2))) inline void errorLogDouble(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  recordLine(gErrorLogs, &gLastError, fmt, args);
  va_end(args);
}
__attribute__((format(printf, 1, 2))) inline void broadcastDouble(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  recordLine(gBroadcasts, &gLastBroadcast, fmt, args);
  va_end(args);
}
inline void broadcastOutput(const char* line) { broadcastDouble("%s", line); }

#define DEBUG_ESPNOWF(...) debugLogDouble(__VA_ARGS__)
#define ERROR_ESPNOWF(...) errorLogDouble(__VA_ARGS__)
#define BROADCAST_PRINTF(...) broadcastDouble(__VA_ARGS__)

inline std::string lastBroadcast() {
  std::lock_guard<std::mutex> lock(gLogMutex);
  return gLastBroadcast;
}
inline std::string lastError() {
  std::lock_guard<std::mutex> lock(gLogMutex);
  return gLastError;
}

// ---- Production types and declarations (System_ESPNow.h) ----------------------
// INSERT_TYPES

// INSERT_DECLS

// Reduced EspNowState: only the message-store members, sliced from the header
// of the tree under test so their types follow that tree.
struct EspNowState {
  bool initialized = true;
  // INSERT_STATE
};

static int gMeshPeerSlots = 16;
static EspNowState gStateStorage;
static EspNowState* gEspNow = nullptr;

// ---- Production message store (System_ESPNow.cpp) -----------------------------
// INSERT_STORE

// INSERT_INIT

#if !HW1_HISTORY_COPY_API
// The screens' read path on a tree without the copy accessor: follow the ref's
// live-ring pointer (OLED/G2 dereferenced CollapsedMsgRef::head) and count by
// walking the history array (G2 countInboxAll/countInboxForPeer).
static bool espnowCopyCollapsedHead(const CollapsedMsgRef& ref, ReceivedTextMessage* out) {
  if (!ref.head) return false;
  std::memcpy(out, ref.head, sizeof(*out));
  return true;
}
static int espnowHistoryMessageCount(const uint8_t* mac) {
  if (!gEspNow || !gEspNow->peerMessageHistories) return 0;
  int total = 0;
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    const PeerMessageHistory& h = gEspNow->peerMessageHistories[i];
    if (!h.active) continue;
    if (!mac) total += h.count;
    else if (std::memcmp(h.peerMac, mac, 6) == 0) return h.count;
  }
  return total;
}
#endif

// ---- Cases ----------------------------------------------------------------------
static const char* gCase = "";
static std::atomic<int> gFailures{0};

#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      std::fprintf(stderr, "FAIL %s: ", gCase);             \
      std::fprintf(stderr, __VA_ARGS__);                    \
      std::fprintf(stderr, "\n");                           \
      gFailures++;                                          \
    }                                                       \
  } while (0)

// Locally administered test addresses (02:...), never a vendor prefix.
static void macFor(int i, uint8_t out[6]) {
  out[0] = 0x02; out[1] = 0x00; out[2] = 0x5A;
  out[3] = 0x00; out[4] = (uint8_t)(i >> 8); out[5] = (uint8_t)i;
}

static bool storeText(uint8_t* mac, const char* name, const char* text) {
  return storeMessageInPeerHistory(mac, name, text, true, MSG_TEXT);
}

static std::string patternText(size_t len, char seed) {
  std::string s;
  for (size_t i = 0; i < len; i++) s += (char)('a' + (seed + i) % 26);
  return s;
}

static void checkLockDiscipline() {
  CHECK(gLogsUnderLock.load() == 0, "%d log line(s) emitted while the store lock was held",
        gLogsUnderLock.load());
  CHECK(gNestedTakes.load() == 0, "%d nested take(s) of the store lock (deadlock on FreeRTOS)",
        gNestedTakes.load());
  CHECK(gLockTakes.load() == gLockGives.load(), "store lock taken %d times but given %d times",
        gLockTakes.load(), gLockGives.load());
}

static void caseRoundTrip() {
  storeInit();
  CHECK(gEspNow->peerHistoryCapacity > 0, "store did not initialise");
  uint8_t A[6], B[6], C[6], U[6];
  macFor(1, A); macFor(2, B); macFor(3, C); macFor(99, U);

  CHECK(storeText(A, "alpha", "r1"), "store r1");
  CHECK(storeSentMessageInPeerHistory(A, "s1", 101), "store s1");
  CHECK(storeText(A, "alpha", "r2"), "store r2");
  CHECK(storeSentMessageInPeerHistory(A, "s2", 102), "store s2");
  CHECK(storeText(A, "alpha", "r3"), "store r3");

  ReceivedTextMessage* out = new ReceivedTextMessage[600];
  int n = getPeerMessages(A, out, 600, 0);
  static const char* kOrder[] = {"r1", "s1", "r2", "s2", "r3"};
  CHECK(n == 5, "getPeerMessages(A) returned %d records, expected 5", n);
  for (int i = 0; i < n && i < 5; i++) {
    CHECK(std::strcmp(out[i].message, kOrder[i]) == 0, "record %d is '%s', expected '%s'", i,
          out[i].message, kOrder[i]);
    CHECK(out[i].isSent == (i % 2 == 1), "record %d has the wrong direction", i);
    if (i) CHECK(out[i].seqNum > out[i - 1].seqNum, "records not ascending by seqNum at %d", i);
  }
  uint32_t since = n >= 3 ? out[2].seqNum : 0;
  n = getPeerMessages(A, out, 600, since);
  CHECK(n == 2 && std::strcmp(out[0].message, "s2") == 0 && std::strcmp(out[1].message, "r3") == 0,
        "paging after seq %u returned %d records", (unsigned)since, n);

  CHECK(storeText(B, "bravo", "b1"), "store b1");
  n = getAllMessages(out, 600, 0);
  CHECK(n == 6, "getAllMessages returned %d records, expected 6", n);
  for (int i = 1; i < n; i++) CHECK(out[i].seqNum > out[i - 1].seqNum, "getAllMessages unsorted at %d", i);

  std::string longText = patternText(450, 3);
  CHECK(storeReceivedMessageChunked(B, "bravo", longText.c_str(), true, MSG_CMD_RESULT, 777),
        "chunked store");
  n = getPeerMessages(B, out, 600, 0);
  CHECK(n == 4, "peer B holds %d records, expected 4 (1 + 3 pieces)", n);
  for (int i = 1; i < n && i < 4; i++) {
    CHECK(out[i].reqId == 777 && out[i].piece == i && out[i].pieceTotal == 3,
          "piece %d: reqId %u piece %u/%u", i, (unsigned)out[i].reqId, out[i].piece, out[i].pieceTotal);
  }

  CollapsedMsgRef* refs = new CollapsedMsgRef[500];
  ReceivedTextMessage copy;
  n = espnowGetConversation(B, refs, 500);
  CHECK(n == 2, "conversation B collapsed to %d messages, expected 2", n);
  if (n == 2) {
    CHECK(refs[1].reqId == 777 && refs[1].partsPresent == 3 && refs[1].partsTotal == 3 && !refs[1].isSent,
          "chunked message collapsed to reqId %u parts %u/%u", (unsigned)refs[1].reqId,
          refs[1].partsPresent, refs[1].partsTotal);
    CHECK(espnowCopyCollapsedHead(refs[1], &copy), "head copy of the chunked message refused");
    CHECK(copy.piece == 1 && std::strlen(copy.message) == 200 &&
          std::strncmp(copy.message, longText.c_str(), 200) == 0,
          "chunked head is piece %u with %zu bytes", copy.piece, std::strlen(copy.message));
  }
  char buf[1200];
  bool complete = false;
  int len = espnowReassembleByReqId(B, 777, false, buf, sizeof(buf), &complete);
  CHECK(len == 450 && longText == buf && complete, "reassembly returned %d bytes, complete=%d", len,
        (int)complete);

  std::string sentLong = patternText(300, 7);
  CHECK(storeSentMessageInPeerHistory(A, sentLong.c_str(), 555), "chunked sent store");
  espnowUpdateSentDeliveryState(A, 555, 1);
  espnowUpdateSentDeliveryState(A, 101, 3);
  n = getPeerMessages(A, out, 600, 0);
  int stamped = 0;
  for (int i = 0; i < n; i++) {
    if (out[i].reqId == 555) {
      stamped++;
      CHECK(out[i].isSent && out[i].sendState == 1, "fragment %u of msg 555 not stamped delivered",
            out[i].piece);
    }
    if (out[i].reqId == 101) CHECK(out[i].sendState == 3, "msg 101 not stamped failed");
  }
  CHECK(stamped == 2, "msg 555 has %d fragments, expected 2", stamped);

  n = espnowGetConversation(A, refs, 500);
  CHECK(n == 6, "conversation A collapsed to %d messages, expected 6", n);
  static const char* kConv[] = {"r1", "s1", "r2", "s2", "r3"};
  for (int i = 0; i < n && i < 6; i++) {
    CHECK(refs[i].isSent == (i % 2 == 1), "conversation entry %d has the wrong direction", i);
    bool ok = espnowCopyCollapsedHead(refs[i], &copy);
    CHECK(ok, "conversation entry %d head copy refused", i);
    if (ok && i < 5) CHECK(std::strcmp(copy.message, kConv[i]) == 0, "entry %d is '%s'", i, copy.message);
    if (ok && i == 5) CHECK(refs[i].partsTotal == 2 && copy.piece == 1, "long send collapsed badly");
  }

  n = espnowCollapsedAllMessages(refs, 500);
  CHECK(n == 8, "all-peers inbox collapsed to %d messages, expected 8", n);
  unsigned long lastStamp = 0;
  for (int i = 0; i < n; i++) {
    bool ok = espnowCopyCollapsedHead(refs[i], &copy);
    CHECK(ok, "inbox entry %d head copy refused", i);
    if (ok) {
      CHECK(copy.timestamp >= lastStamp, "inbox not ordered by time at %d", i);
      lastStamp = copy.timestamp;
    }
  }
  n = espnowCollapsedPeerMessages(B, refs, 500);
  CHECK(n == 2, "received-only collapse for B returned %d, expected 2", n);

  char text[16];
  for (int i = 0; i < 260; i++) {
    std::snprintf(text, sizeof(text), "m%d", i);
    storeText(C, "charlie", text);
  }
  n = getPeerMessages(C, out, 600, 0);
  CHECK(n == MESSAGES_PER_DEVICE, "wrapped ring holds %d records, expected %d", n, MESSAGES_PER_DEVICE);
  if (n == MESSAGES_PER_DEVICE) {
    CHECK(std::strcmp(out[0].message, "m10") == 0 && std::strcmp(out[n - 1].message, "m259") == 0,
          "ring wrap kept '%s'..'%s', expected m10..m259", out[0].message, out[n - 1].message);
  }

  CHECK(espnowHistoryMessageCount(nullptr) == 3 + 4 + MESSAGES_PER_DEVICE,
        "received count over all peers is %d, expected %d", espnowHistoryMessageCount(nullptr),
        3 + 4 + MESSAGES_PER_DEVICE);
  CHECK(espnowHistoryMessageCount(A) == 3, "received count for A is %d", espnowHistoryMessageCount(A));
  CHECK(espnowHistoryMessageCount(U) == 0, "received count for an unknown peer is %d",
        espnowHistoryMessageCount(U));
  checkLockDiscipline();
  delete[] out;
  delete[] refs;
}

static void caseGrowth() {
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  CHECK(storeText(A, "alpha", "hello"), "store hello");
  CollapsedMsgRef refs[8];
  int n = espnowGetConversation(A, refs, 8);
  CHECK(n == 1, "conversation A has %d messages", n);
  if (n != 1) return;
  ReceivedTextMessage before, after;
  CHECK(espnowCopyCollapsedHead(refs[0], &before), "fresh ref refused");
  // A UI holds refs[0] while six previously unseen peers send text (more than
  // the PEER_HISTORY_INITIAL_CAPACITY slots allocated at init).
  for (int i = 1; i <= 6; i++) {
    uint8_t mac[6];
    macFor(100 + i, mac);
    CHECK(storeText(mac, "stranger", "x"), "store from new peer %d", i);
  }
  bool ok = espnowCopyCollapsedHead(refs[0], &after);
  CHECK(ok, "a ref taken before six new peers arrived no longer reads its record");
  if (ok) {
    CHECK(after.seqNum == before.seqNum && std::strcmp(after.message, "hello") == 0,
          "a ref taken before six new peers arrived reads seq %u text '%.12s' (expected seq %u 'hello'):"
          " the history array moved under it", (unsigned)after.seqNum, after.message,
          (unsigned)before.seqNum);
  }
  ReceivedTextMessage out[4];
  n = getPeerMessages(A, out, 4, 0);
  CHECK(n == 1 && std::strcmp(out[0].message, "hello") == 0, "peer A lost its record after growth");
  checkLockDiscipline();
}

static void caseFindOnly() {
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  CHECK(storeText(A, "alpha", "hello"), "store hello");
  const int slots0 = gEspNow->peerHistoryCount;
  const int allocs0 = gAllocCalls.load();
  ReceivedTextMessage out[4];
  CollapsedMsgRef refs[8];
  char buf[64];
  for (int i = 0; i < 20; i++) {
    uint8_t mac[6];
    macFor(500 + i, mac);
    CHECK(getPeerMessages(mac, out, 4, 0) == 0, "unknown peer has messages");
    CHECK(espnowGetConversation(mac, refs, 8) == 0, "unknown peer has a conversation");
    CHECK(espnowCollapsedPeerMessages(mac, refs, 8) == 0, "unknown peer has collapsed messages");
    CHECK(espnowReassembleByReqId(mac, 7, false, buf, sizeof(buf)) == 0, "unknown peer reassembled");
    espnowUpdateSentDeliveryState(mac, 7, 1);
  }
  CHECK(gEspNow->peerHistoryCount == slots0,
        "reads for 20 unknown MACs created %d peer history slot(s) (read API creates slots)",
        gEspNow->peerHistoryCount - slots0);
  CHECK(gAllocCalls.load() == allocs0,
        "reads for 20 unknown MACs allocated %d time(s) (read API grows the history)",
        gAllocCalls.load() - allocs0);
  checkLockDiscipline();
}

#if HW1_HISTORY_COPY_API
static void caseBounded() {
  storeInit();
  const int allocsInit = gAllocCalls.load();
  CHECK(gEspNow->peerHistoryCapacity == gMeshPeerSlots, "slot table has %d entries, expected %d",
        gEspNow->peerHistoryCapacity, gMeshPeerSlots);
  int eager = 0;
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) eager += gEspNow->peerMessageHistories[i] ? 1 : 0;
  CHECK(eager == PEER_HISTORY_INITIAL_CAPACITY, "%d blocks allocated at init, expected %d", eager,
        PEER_HISTORY_INITIAL_CAPACITY);

  uint8_t macs[17][6];
  for (int i = 0; i < 17; i++) macFor(10 + i, macs[i]);
  for (int i = 0; i < PEER_HISTORY_INITIAL_CAPACITY; i++) CHECK(storeText(macs[i], "p", "a"), "store %d", i);
  CHECK(gAllocCalls.load() == allocsInit, "peers in eager slots allocated");

  // A new peer whose block cannot be allocated is refused and leaves no slot.
  gAllocFail = true;
  const int errors0 = gErrorLogs.load();
  CHECK(!storeText(macs[5], "p", "a"), "store succeeded without a block");
  gAllocFail = false;
  CHECK(gErrorLogs.load() == errors0 + 1 &&
        lastError().find("Failed to allocate peer history block") != std::string::npos,
        "block failure not logged (last error '%s')", lastError().c_str());
  CHECK(lastBroadcast().find("No free peer history slots") != std::string::npos,
        "refused store not broadcast (last line '%s')", lastBroadcast().c_str());
  CHECK(gEspNow->peerHistoryCount == PEER_HISTORY_INITIAL_CAPACITY, "failed store activated a slot");
  CHECK(storeText(macs[5], "p", "a"), "retry after the allocation failure");
  CHECK(lastBroadcast().find("Peer history slot 6/16 allocated") != std::string::npos,
        "new block not reported (last line '%s')", lastBroadcast().c_str());

  for (int i = 6; i < 16; i++) CHECK(storeText(macs[i], "p", "a"), "store %d", i);
  CHECK(gEspNow->peerHistoryCount == 16, "%d active slots, expected 16", gEspNow->peerHistoryCount);
  const int allocsFull = gAllocCalls.load();
  CHECK(allocsFull - allocsInit == 16 - PEER_HISTORY_INITIAL_CAPACITY + 1,
        "%d block allocations for 11 new peers (+1 failed)", allocsFull - allocsInit);

  PeerMessageHistory* blocks[16];
  for (int i = 0; i < 16; i++) blocks[i] = gEspNow->peerMessageHistories[i];

  CHECK(!storeText(macs[16], "p", "a"), "17th peer stored past the slot limit");
  CHECK(lastBroadcast().find("No free peer history slots") != std::string::npos,
        "17th peer refusal not broadcast");
  CHECK(gAllocCalls.load() == allocsFull, "17th peer allocated");

  CollapsedMsgRef refs[8];
  int n = espnowGetConversation(macs[0], refs, 8);
  CHECK(n == 1, "peer 0 conversation has %d messages", n);
  for (int i = 0; i < 1000; i++) storeText(macs[i % 16], "p", "b");
  for (int i = 0; i < 16; i++) {
    CHECK(gEspNow->peerMessageHistories[i] == blocks[i], "slot %d block moved after 1000 stores", i);
  }
  ReceivedTextMessage copy;
  CHECK(espnowCopyCollapsedHead(refs[0], &copy), "ref refused before its record was overwritten");
  for (int i = 0; i < MESSAGES_PER_DEVICE; i++) storeText(macs[0], "p", "c");
  CHECK(!espnowCopyCollapsedHead(refs[0], &copy),
        "a ref whose ring index now holds a newer record was accepted");
  CollapsedMsgRef bad = refs[0];
  bad.slot = 200;
  CHECK(!espnowCopyCollapsedHead(bad, &copy), "ref with an out-of-range slot accepted");
  bad = refs[0];
  bad.index = 255;
  CHECK(!espnowCopyCollapsedHead(bad, &copy), "ref with an out-of-range index accepted");
  n = espnowGetConversation(macs[0], refs, 8);
  CHECK(n == 8 && espnowCopyCollapsedHead(refs[0], &copy), "fresh refs refused after the wrap");
  checkLockDiscipline();
}

static void caseLockRefusal() {
  storeInit();
  uint8_t A[6], N[6];
  macFor(1, A);
  macFor(2, N);
  CHECK(storeText(A, "alpha", "one"), "store one");
  CollapsedMsgRef refs[8];
  CHECK(espnowGetConversation(A, refs, 8) == 1, "conversation before refusal");
  const int slots0 = gEspNow->peerHistoryCount;
  const uint32_t seq0 = gEspNow->globalMessageSeqNum;
  const int allocs0 = gAllocCalls.load();
  const int errors0 = gErrorLogs.load();

  gLockRefuse = true;
  CHECK(!storeText(A, "alpha", "two"), "store with the lock refused succeeded");
  CHECK(!storeText(N, "new", "two"), "store for a new peer with the lock refused succeeded");
  CHECK(!storeSentMessageInPeerHistory(A, "two", 9), "sent store with the lock refused succeeded");
  CHECK(!storeReceivedMessageChunked(A, "alpha", "two", true, MSG_TEXT, 9),
        "chunked store with the lock refused succeeded");
  CHECK(gErrorLogs.load() == errors0 + 4 && lastError().find("busy") != std::string::npos,
        "refused stores logged %d error(s), last '%s'", gErrorLogs.load() - errors0, lastError().c_str());
  ReceivedTextMessage out[4], copy;
  char buf[64];
  CHECK(getPeerMessages(A, out, 4, 0) == 0, "read with the lock refused returned records");
  CHECK(getAllMessages(out, 4, 0) == 0, "all-read with the lock refused returned records");
  CHECK(espnowGetConversation(A, refs, 8) == 0, "conversation with the lock refused");
  CHECK(espnowCollapsedPeerMessages(A, refs, 8) == 0, "collapse with the lock refused");
  CHECK(espnowCollapsedAllMessages(refs, 8) == 0, "inbox with the lock refused");
  CHECK(espnowReassembleByReqId(A, 9, false, buf, sizeof(buf)) == 0, "reassembly with the lock refused");
  CHECK(!espnowCopyCollapsedHead(refs[0], &copy), "head copy with the lock refused");
  CHECK(espnowHistoryMessageCount(nullptr) == 0, "count with the lock refused");
  espnowUpdateSentDeliveryState(A, 9, 1);
  gLockRefuse = false;
  CHECK(gEspNow->peerHistoryCount == slots0 && gEspNow->globalMessageSeqNum == seq0 &&
        gAllocCalls.load() == allocs0, "refused calls changed the store");

  CHECK(storeText(A, "alpha", "two"), "store after the refusal cleared");
  CHECK(getPeerMessages(A, out, 4, 0) == 2, "store not readable after the refusal cleared");

  // Real contention: another task holds the lock past the timeout.
  std::atomic<bool> holding{false};
  std::thread holder([&] {
    if (xSemaphoreTake(gPeerHistoryLock, 1000) == pdTRUE) {
      holding = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(PEER_HISTORY_LOCK_TIMEOUT_MS + 200));
      xSemaphoreGive(gPeerHistoryLock);
    }
  });
  while (!holding.load()) std::this_thread::yield();
  auto t0 = std::chrono::steady_clock::now();
  bool stored = storeText(A, "alpha", "three");
  auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
  holder.join();
  CHECK(!stored, "store succeeded while another task held the lock");
  CHECK(waited.count() >= PEER_HISTORY_LOCK_TIMEOUT_MS / 2, "store gave up after %lld ms",
        (long long)waited.count());
  CHECK(storeText(A, "alpha", "three"), "store after the holder released");
  CHECK(getPeerMessages(A, out, 4, 0) == 3, "store after contention not readable");
  checkLockDiscipline();
}
#endif  // HW1_HISTORY_COPY_API

static void caseRace() {
  storeInit();
  uint8_t macs[12][6];
  for (int i = 0; i < 12; i++) macFor(300 + i, macs[i]);
  const std::string longText = patternText(450, 1);
  std::atomic<bool> stop{false};
  std::atomic<int> writes{0}, reads{0}, badRecords{0};
  std::atomic<uint32_t> maxSeqSeen{0};

  auto noteRecord = [&](const ReceivedTextMessage& r) {
    if (!r.active || r.seqNum == 0 || !std::memchr(r.message, '\0', sizeof(r.message)) ||
        !std::memchr(r.senderName, '\0', sizeof(r.senderName))) {
      badRecords++;
    }
    uint32_t seen = maxSeqSeen.load();
    while (r.seqNum > seen && !maxSeqSeen.compare_exchange_weak(seen, r.seqNum)) {}
  };

  std::thread writer([&] {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    for (uint32_t i = 1; std::chrono::steady_clock::now() < end; i++) {
      uint8_t* mac = macs[i % 12];
      switch (i % 4) {
        case 0: storeText(mac, "racer", "hello"); break;
        case 1: storeSentMessageInPeerHistory(mac, "reply", i); break;
        case 2: storeReceivedMessageChunked(mac, "racer", longText.c_str(), true, MSG_CMD_RESULT, i); break;
        default: espnowUpdateSentDeliveryState(mac, i - 2, 1); break;
      }
      writes++;
    }
    stop = true;
  });

  std::thread reader([&] {
    ReceivedTextMessage* out = new ReceivedTextMessage[128];
    CollapsedMsgRef* refs = new CollapsedMsgRef[2 * MESSAGES_PER_DEVICE];
    ReceivedTextMessage copy;
    char buf[1200];
    for (int k = 0; !stop.load(); k++) {
      int n = getAllMessages(out, 128, 0);
      for (int i = 0; i < n; i++) noteRecord(out[i]);
      n = getPeerMessages(macs[k % 12], out, 128, 0);
      for (int i = 0; i < n; i++) noteRecord(out[i]);
      n = espnowGetConversation(macs[k % 12], refs, 2 * MESSAGES_PER_DEVICE);
      for (int i = 0; i < n; i += 7) {
        if (espnowCopyCollapsedHead(refs[i], &copy)) noteRecord(copy);
      }
      n = espnowCollapsedAllMessages(refs, 2 * MESSAGES_PER_DEVICE);
      for (int i = 0; i < n; i += 11) {
        if (espnowCopyCollapsedHead(refs[i], &copy)) {
          noteRecord(copy);
          if (refs[i].reqId) {
            int len = espnowReassembleByReqId(copy.senderMac, refs[i].reqId, refs[i].isSent, buf, sizeof(buf));
            if (len < 0 || (size_t)len >= sizeof(buf) || std::strlen(buf) != (size_t)len) badRecords++;
          }
        }
      }
      int count = espnowHistoryMessageCount(nullptr);
      if (count < 0 || count > 12 * MESSAGES_PER_DEVICE) badRecords++;
      reads++;
    }
    delete[] out;
    delete[] refs;
  });

  writer.join();
  reader.join();
  std::printf("race: %d store calls, %d reader rounds\n", writes.load(), reads.load());
  CHECK(writes.load() > 0 && reads.load() > 0, "race did not run (%d writes, %d reads)", writes.load(),
        reads.load());
  CHECK(badRecords.load() == 0, "%d torn or inactive record(s) read during concurrent stores",
        badRecords.load());
  CHECK(maxSeqSeen.load() <= gEspNow->globalMessageSeqNum, "read seq %u beyond the store's %u",
        (unsigned)maxSeqSeen.load(), (unsigned)gEspNow->globalMessageSeqNum);
  checkLockDiscipline();
}

int main(int argc, char** argv) {
  gEspNow = &gStateStorage;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return 2;
  }
  gCase = argv[1];
  const std::string which = argv[1];
  if (which == "roundtrip") caseRoundTrip();
  else if (which == "growth") caseGrowth();
  else if (which == "find_only") caseFindOnly();
#if HW1_HISTORY_COPY_API
  else if (which == "bounded") caseBounded();
  else if (which == "lock_refusal") caseLockRefusal();
#else
  else if (which == "bounded" || which == "lock_refusal") {
    CHECK(false, "this tree has no store lock, slot table or espnowCopyCollapsedHead to test");
  }
#endif
  else if (which == "race") caseRace();
  else {
    std::fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
  }
  if (gFailures.load()) return 1;
  std::printf("ok %s\n", argv[1]);
  return 0;
}
