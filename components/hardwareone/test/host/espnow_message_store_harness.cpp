// Host harness for the ESP-NOW peer message store (System_ESPNow.cpp, section
// "Per-Device Message Buffer Management"). test_espnow_message_store.py inserts
// the production history macros, types, declarations, EspNowState members and
// constructor items, and the whole store section; this file supplies the
// FreeRTOS mutex, allocation, heap, clock, settings and logging doubles and the
// cases. Each case runs in its own process (argv[1]) so a sanitizer abort in
// one case cannot hide the others. The ring itself is the production
// System_ESPNow_HistoryCore.h (copied beside this unit).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "System_MacText.h"            // production MAC text helpers (copied beside this unit)
#include "System_ESPNow_HistoryCore.h"  // production ring (copied beside this unit)

// ---- Build configuration the store sees on a PSRAM build ---------------------
#define CONFIG_SPIRAM 1
#define MESH_PEER_MAX 16
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

// ---- Allocation, heap, clock, settings and logging doubles --------------------
enum class AllocPref { PreferPsram, PreferInternal, RequirePsram, RequireInternal };

static std::atomic<int> gAllocCalls{0};
static std::atomic<int> gFreeCalls{0};
static std::atomic<int> gAllocUnderLock{0};
static std::atomic<int> gFreeUnderLock{0};
static std::atomic<bool> gAllocFail{false};
static std::mutex gAllocMutex;
static std::string gAllocFailTag;                // fail only this tag ("" = none)
static std::map<std::string, int> gAllocTags;    // successful + failed calls per tag
static std::map<std::string, size_t> gAllocBytes;
static std::map<std::string, AllocPref> gAllocPolicy;
static std::map<void*, size_t> gLive;
static std::function<void()> gAllocHook;        // runs after an allocation (no lock held)

inline void* ps_alloc(size_t size, AllocPref policy, const char* tag) {
  gAllocCalls++;
  if (lockHeldByMe()) gAllocUnderLock++;  // allocation logs and tracks: never under the store lock
  const std::string t = tag ? tag : "(null)";
  bool fail = gAllocFail.load();
  {
    std::lock_guard<std::mutex> lock(gAllocMutex);
    gAllocTags[t]++;
    gAllocPolicy[t] = policy;
    if (!gAllocFailTag.empty() && gAllocFailTag == t) fail = true;
  }
  if (fail) return nullptr;
  void* p = std::malloc(size);
  std::memset(p, 0xA5, size);  // never trust fresh memory
  {
    std::lock_guard<std::mutex> lock(gAllocMutex);
    gAllocBytes[t] += size;
    gLive[p] = size;
  }
  if (gAllocHook) {
    std::function<void()> hook;
    hook.swap(gAllocHook);  // one shot
    hook();
  }
  return p;
}
inline void ps_free(void* p) {
  if (!p) return;
  gFreeCalls++;
  if (lockHeldByMe()) gFreeUnderLock++;
  {
    std::lock_guard<std::mutex> lock(gAllocMutex);
    gLive.erase(p);
  }
  std::free(p);
}
static int allocsOf(const char* tag) {
  std::lock_guard<std::mutex> lock(gAllocMutex);
  auto it = gAllocTags.find(tag);
  return it == gAllocTags.end() ? 0 : it->second;
}
static size_t bytesOf(const char* tag) {
  std::lock_guard<std::mutex> lock(gAllocMutex);
  auto it = gAllocBytes.find(tag);
  return it == gAllocBytes.end() ? 0 : it->second;
}

static std::atomic<bool> gPsram{true};
static std::atomic<size_t> gTotalSpiram{8u * 1024u * 1024u};
// Internal heap as on the classic ESP32: the 8-bit-capable DRAM (gFreeInternal,
// gLargestInternal) plus an IRAM-only region (INTERNAL | 32BIT, no 8BIT) that
// ps_alloc(RequireInternal) cannot use. A query for MALLOC_CAP_INTERNAL alone
// counts both, as ESP-IDF does.
static std::atomic<size_t> gFreeInternal{200u * 1024u};
static std::atomic<size_t> gLargestInternal{200u * 1024u};
static std::atomic<size_t> gFreeIramOnly{0};
static std::atomic<size_t> gLargestIramOnly{0};
#define MALLOC_CAP_8BIT 0x4u
#define MALLOC_CAP_INTERNAL 0x800u
#define MALLOC_CAP_SPIRAM 0x400u
inline bool psramAvailableRuntime() { return gPsram.load(); }
inline size_t heap_caps_get_total_size(uint32_t caps) { return caps == MALLOC_CAP_SPIRAM ? gTotalSpiram.load() : 0; }
inline size_t heap_caps_get_free_size(uint32_t caps) {
  if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return gFreeInternal.load();
  if (caps == MALLOC_CAP_INTERNAL) return gFreeInternal.load() + gFreeIramOnly.load();
  return 0;
}
inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
  if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return gLargestInternal.load();
  if (caps == MALLOC_CAP_INTERNAL) return std::max(gLargestInternal.load(), gLargestIramOnly.load());
  return 0;
}

static std::atomic<unsigned long> gMillis{1000};
inline unsigned long millis() { return gMillis.fetch_add(1) + 1; }
inline unsigned long micros() { return gMillis.load() * 1000ul; }
static void advanceMs(unsigned long ms) { gMillis += ms; }

static struct { uint8_t espnowHistoryKb = 0; } gSettings;

static std::atomic<int> gLogsUnderLock{0};
static std::atomic<int> gDebugLogs{0};
static std::atomic<int> gErrorLogs{0};
static std::atomic<int> gBroadcasts{0};
static std::mutex gLogMutex;
static std::string gLastBroadcast;
static std::string gLastError;
static std::vector<std::string> gBroadcastLines;

static void recordLine(std::atomic<int>& counter, std::string* last, const char* fmt, va_list args) {
  if (lockHeldByMe()) gLogsUnderLock++;  // the store lock must stay a leaf
  counter++;
  char line[512];
  std::vsnprintf(line, sizeof(line), fmt, args);
  if (last) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    *last = line;
    if (last == &gLastBroadcast) gBroadcastLines.push_back(line);
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
// Broadcast lines since index `from` that contain `needle`.
static int linesWith(size_t from, const char* needle, std::string* last = nullptr) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  int n = 0;
  for (size_t i = from; i < gBroadcastLines.size(); i++) {
    if (gBroadcastLines[i].find(needle) != std::string::npos) {
      n++;
      if (last) *last = gBroadcastLines[i];
    }
  }
  return n;
}
static size_t lineMark() {
  std::lock_guard<std::mutex> lock(gLogMutex);
  return gBroadcastLines.size();
}

// ---- Production types and declarations (System_ESPNow.h) ----------------------
// INSERT_TYPES

// INSERT_DECLS

// Reduced EspNowState: only the message-store members, sliced from the header
// of the tree under test, with the production constructor's items for them.
struct EspNowState {
  bool initialized = true;
  // INSERT_STATE
  // INSERT_CTOR
};

static int gMeshPeerSlots = 16;
static EspNowState* gEspNow = nullptr;

// ---- Production message store (System_ESPNow.cpp) -----------------------------
// INSERT_STORE

// INSERT_INIT

// ---- Cases ----------------------------------------------------------------------
static const char* gCase = "";
static std::atomic<int> gFailures{0};
static std::atomic<long> gChecks{0};

#define CHECK(cond, ...)                                    \
  do {                                                      \
    gChecks++;                                              \
    if (!(cond)) {                                          \
      std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, gCase); \
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
static std::string macText(const uint8_t* mac) {
  char buf[18];
  macToDisplay(mac, buf, sizeof(buf));
  return buf;
}

static bool storeText(uint8_t* mac, const char* name, const char* text) {
  return storeMessageInPeerHistory(mac, name, text, true, MSG_TEXT);
}

static std::string patternText(size_t len, char seed) {
  std::string s;
  for (size_t i = 0; i < len; i++) s += (char)('a' + (seed + i) % 26);
  return s;
}

static PeerMessageHistory* blockOf(const uint8_t* mac) {
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    PeerMessageHistory* h = gEspNow->peerMessageHistories[i];
    if (h && std::memcmp(h->peerMac, mac, 6) == 0) return h;
  }
  return nullptr;
}
static size_t blockBytes() { return sizeof(PeerMessageHistory) + 2u * gEspNow->peerHistoryRingBytes; }

// Every view is NUL-terminated and its length matches.
static bool viewOk(const ReceivedTextMessage& r) {
  const void* nul = std::memchr(r.message, '\0', sizeof(r.message));
  return r.seqNum != 0 && nul && std::strlen(r.message) == r.length;
}

static void checkLockDiscipline() {
  CHECK(gLogsUnderLock.load() == 0, "%d log line(s) emitted while the store lock was held",
        gLogsUnderLock.load());
  CHECK(gNestedTakes.load() == 0, "%d nested take(s) of the store lock (deadlock on FreeRTOS)",
        gNestedTakes.load());
  CHECK(gLockTakes.load() == gLockGives.load(), "store lock taken %d times but given %d times",
        gLockTakes.load(), gLockGives.load());
  CHECK(gAllocUnderLock.load() == 0, "%d allocation(s) while the store lock was held", gAllocUnderLock.load());
  CHECK(gFreeUnderLock.load() == 0, "%d free(s) while the store lock was held", gFreeUnderLock.load());
}

// The state is built with its real constructor items on 0xA5-filled memory
// (the device places gEspNow on ps_alloc memory).
alignas(EspNowState) static unsigned char gStateMemory[sizeof(EspNowState)];
static void buildState() {
  std::memset(gStateMemory, 0xA5, sizeof(gStateMemory));
  gEspNow = new (gStateMemory) EspNowState();
}

static void caseCtor() {
  bool tableNull = true;
  for (int i = 0; i < MESH_PEER_MAX; i++) tableNull = tableNull && gEspNow->peerMessageHistories[i] == nullptr;
  CHECK(tableNull, "constructor left a slot pointer set");
  CHECK(gEspNow->peerHistoryCapacity == 0 && gEspNow->peerHistoryCount == 0 &&
        gEspNow->peerHistoryRingBytes == 0 && gEspNow->globalMessageSeqNum == 0,
        "constructor left capacity %d count %d ring %u seq %u", gEspNow->peerHistoryCapacity,
        gEspNow->peerHistoryCount, (unsigned)gEspNow->peerHistoryRingBytes, (unsigned)gEspNow->globalMessageSeqNum);
  uint8_t A[6];
  macFor(1, A);
  ReceivedTextMessage out[4];
  CollapsedMsgRef refs[4] = {};
  char buf[32];
  char status[160];
  espnowHistoryStatusLine(status, sizeof(status));
  CHECK(std::strstr(status, "not started") != nullptr, "status before init: '%s'", status);
  CHECK(!storeText(A, "alpha", "x"), "store before init succeeded");
  CHECK(!storeSentMessageInPeerHistory(A, "x", 5), "sent store before init succeeded");
  CHECK(!storeReceivedMessageChunked(A, "alpha", "x", true, MSG_TEXT, 5), "chunked store before init");
  CHECK(getPeerMessages(A, out, 4, 0) == 0 && getAllMessages(out, 4, 0) == 0, "reads before init");
  CHECK(espnowGetConversation(A, refs, 4) == 0 && espnowCollapsedAllMessages(refs, 4) == 0 &&
        espnowCollapsedPeerMessages(A, refs, 4) == 0, "collapses before init");
  CHECK(espnowReassembleByReqId(A, 5, false, buf, sizeof(buf)) == 0 && !espnowCopyCollapsedHead(refs[0], out) &&
        espnowHistoryMessageCount(nullptr) == 0, "other reads before init");
  espnowUpdateSentDeliveryState(A, 5, 1);
  CHECK(gAllocCalls.load() == 0 && gEspNow->globalMessageSeqNum == 0, "calls before init touched the store");

  const size_t mark = lineMark();
  storeInit(0);
  CHECK(gAllocCalls.load() == 0, "init allocated %d time(s)", gAllocCalls.load());
  CHECK(gEspNow->peerHistoryCapacity == 16 && gEspNow->peerHistoryRingBytes == 16384,
        "init: capacity %d ring %u", gEspNow->peerHistoryCapacity, (unsigned)gEspNow->peerHistoryRingBytes);
  std::string start;
  CHECK(linesWith(mark, "[ESP-NOW] Message history: ", &start) == 1, "start line said %d time(s)",
        linesWith(mark, "[ESP-NOW] Message history: "));
  CHECK(start.find("32 KB per peer (16 KB received + 16 KB sent) in PSRAM") != std::string::npos &&
        start.find("up to 16 peers") != std::string::npos, "start line '%s'", start.c_str());
  espnowHistoryStatusLine(status, sizeof(status));
  CHECK(std::strstr(status, "saved 0 KB (0 = auto), active 16 KB per ring, 0/16 peers, 0 KB in use"),
        "status after init: '%s'", status);
  // Initialised, but no block and no scratch yet: every read returns nothing
  // (and touches no scratch; a UBSan report fails the case).
  CHECK(getPeerMessages(A, out, 4, 0) == 0 && getAllMessages(out, 4, 0) == 0, "reads before the first block");
  CHECK(espnowGetConversation(A, refs, 4) == 0 && espnowCollapsedAllMessages(refs, 4) == 0 &&
        espnowCollapsedPeerMessages(A, refs, 4) == 0, "collapses before the first block");
  CHECK(espnowReassembleByReqId(A, 5, false, buf, sizeof(buf)) == 0 && !espnowCopyCollapsedHead(refs[0], out) &&
        espnowHistoryMessageCount(nullptr) == 0, "other reads before the first block");
  espnowUpdateSentDeliveryState(A, 5, 1);
  CHECK(gAllocCalls.load() == 0, "reads before the first block allocated");
  gSettings.espnowHistoryKb = 32;
  storeInit(32);  // a second init changes nothing (latched once per boot)
  CHECK(gEspNow->peerHistoryRingBytes == 16384 && linesWith(mark, "[ESP-NOW] Message history: ") == 1,
        "second init changed the store");
  espnowHistoryStatusLine(status, sizeof(status));
  CHECK(std::strstr(status, "saved 32 KB") && std::strstr(status, "active 16 KB"),
        "status shows saved and active: '%s'", status);
  espnowHistoryStatusLine(status, 1);
  CHECK(status[0] == '\0', "status with cap 1 wrote text");
  checkLockDiscipline();
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

  ReceivedTextMessage* out = new ReceivedTextMessage[2000];
  int n = getPeerMessages(A, out, 2000, 0);
  static const char* kOrder[] = {"r1", "s1", "r2", "s2", "r3"};
  CHECK(n == 5, "getPeerMessages(A) returned %d records, expected 5", n);
  for (int i = 0; i < n && i < 5; i++) {
    CHECK(std::strcmp(out[i].message, kOrder[i]) == 0, "record %d is '%s', expected '%s'", i,
          out[i].message, kOrder[i]);
    CHECK(out[i].isSent == (i % 2 == 1), "record %d has the wrong direction", i);
    CHECK(std::memcmp(out[i].peerMac, A, 6) == 0 && viewOk(out[i]), "record %d view", i);
    if (i) CHECK(out[i].seqNum > out[i - 1].seqNum, "records not ascending by seqNum at %d", i);
  }
  uint32_t since = n >= 3 ? out[2].seqNum : 0;
  n = getPeerMessages(A, out, 2000, since);
  CHECK(n == 2 && std::strcmp(out[0].message, "s2") == 0 && std::strcmp(out[1].message, "r3") == 0,
        "paging after seq %u returned %d records", (unsigned)since, n);

  CHECK(storeText(B, "bravo", "b1"), "store b1");
  n = getAllMessages(out, 2000, 0);
  CHECK(n == 6, "getAllMessages returned %d records, expected 6", n);
  for (int i = 1; i < n; i++) CHECK(out[i].seqNum > out[i - 1].seqNum, "getAllMessages unsorted at %d", i);

  std::string longText = patternText(450, 3);
  CHECK(storeReceivedMessageChunked(B, "bravo", longText.c_str(), true, MSG_CMD_RESULT, 777),
        "chunked store");
  n = getPeerMessages(B, out, 2000, 0);
  CHECK(n == 4, "peer B holds %d records, expected 4 (1 + 3 pieces)", n);
  for (int i = 1; i < n && i < 4; i++) {
    CHECK(out[i].reqId == 777 && out[i].piece == i && out[i].pieceTotal == 3 && out[i].msgType == MSG_CMD_RESULT,
          "piece %d: reqId %u piece %u/%u", i, (unsigned)out[i].reqId, out[i].piece, out[i].pieceTotal);
  }

  CollapsedMsgRef* refs = new CollapsedMsgRef[ESPNOW_HISTORY_VIEW_MAX];
  ReceivedTextMessage copy;
  n = espnowGetConversation(B, refs, ESPNOW_HISTORY_VIEW_MAX);
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
  len = espnowReassembleByReqId(B, 777, false, buf, 11, &complete);
  CHECK(len == 10 && std::strncmp(buf, longText.c_str(), 10) == 0 && buf[10] == '\0',
        "reassembly into 11 bytes returned %d", len);

  std::string sentLong = patternText(300, 7);
  CHECK(storeSentMessageInPeerHistory(A, sentLong.c_str(), 555), "chunked sent store");
  espnowUpdateSentDeliveryState(A, 555, 1);
  espnowUpdateSentDeliveryState(A, 101, 3);
  n = getPeerMessages(A, out, 2000, 0);
  int stamped = 0;
  for (int i = 0; i < n; i++) {
    if (out[i].reqId == 555) {
      stamped++;
      CHECK(out[i].isSent && out[i].sendState == 1, "fragment %u of msg 555 not stamped delivered",
            out[i].piece);
    }
    if (out[i].reqId == 101) CHECK(out[i].sendState == 3, "msg 101 not stamped failed");
    if (out[i].reqId == 102) CHECK(out[i].sendState == 0, "msg 102 stamped by another msgId");
  }
  CHECK(stamped == 2, "msg 555 has %d fragments, expected 2", stamped);
  len = espnowReassembleByReqId(A, 555, true, buf, sizeof(buf), &complete);
  CHECK(len == 300 && sentLong == buf && complete, "sent reassembly returned %d", len);

  n = espnowGetConversation(A, refs, ESPNOW_HISTORY_VIEW_MAX);
  CHECK(n == 6, "conversation A collapsed to %d messages, expected 6", n);
  static const char* kConv[] = {"r1", "s1", "r2", "s2", "r3"};
  for (int i = 0; i < n && i < 6; i++) {
    CHECK(refs[i].isSent == (i % 2 == 1), "conversation entry %d has the wrong direction", i);
    bool ok = espnowCopyCollapsedHead(refs[i], &copy);
    CHECK(ok, "conversation entry %d head copy refused", i);
    if (ok && i < 5) CHECK(std::strcmp(copy.message, kConv[i]) == 0, "entry %d is '%s'", i, copy.message);
    if (ok && i == 5) CHECK(refs[i].partsTotal == 2 && copy.piece == 1, "long send collapsed badly");
  }

  n = espnowCollapsedAllMessages(refs, ESPNOW_HISTORY_VIEW_MAX);
  CHECK(n == 8, "all-peers inbox collapsed to %d messages, expected 8", n);
  uint32_t lastStamp = 0;
  for (int i = 0; i < n; i++) {
    bool ok = espnowCopyCollapsedHead(refs[i], &copy);
    CHECK(ok, "inbox entry %d head copy refused", i);
    if (ok) {
      CHECK(copy.timestamp >= lastStamp, "inbox not ordered by time at %d", i);
      lastStamp = copy.timestamp;
    }
  }
  n = espnowCollapsedPeerMessages(B, refs, ESPNOW_HISTORY_VIEW_MAX);
  CHECK(n == 2, "received-only collapse for B returned %d, expected 2", n);
  // A larger maxN is clamped to ESPNOW_HISTORY_VIEW_MAX (the caller sizes at it).
  n = espnowCollapsedAllMessages(refs, ESPNOW_HISTORY_VIEW_MAX);
  CHECK(n == 8, "clamped inbox returned %d", n);

  // Many short stores into one 16 KB ring: the kept records are a contiguous
  // suffix ending at the newest, as many as the ring's bytes hold.
  char text[16];
  const int kStores = 1000;
  for (int i = 0; i < kStores; i++) {
    std::snprintf(text, sizeof(text), "m%d", i);
    storeText(C, "charlie", text);
  }
  n = getPeerMessages(C, out, 2000, 0);
  PeerMessageHistory* hc = blockOf(C);
  CHECK(hc && n == (int)hc->received.count && n > 500 && n < kStores,
        "ring C holds %d records (core count %d)", n, hc ? (int)hc->received.count : -1);
  if (n > 0) {
    int first = std::atoi(out[0].message + 1);
    CHECK(first == kStores - n, "kept records start at m%d, expected m%d", first, kStores - n);
    bool contiguous = true;
    for (int i = 0; i < n; i++) contiguous = contiguous && std::atoi(out[i].message + 1) == first + i;
    CHECK(contiguous && std::strcmp(out[n - 1].message, "m999") == 0, "kept records not a contiguous suffix");
  }
  if (hc) CHECK(hc->received.used <= hc->received.size, "ring uses %u of %u bytes", (unsigned)hc->received.used,
                (unsigned)hc->received.size);

  const int keptC = n;
  CHECK(espnowHistoryMessageCount(nullptr) == 3 + 4 + keptC,
        "received count over all peers is %d, expected %d", espnowHistoryMessageCount(nullptr), 3 + 4 + keptC);
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
  // A UI holds refs[0] while six previously unseen peers send text (each gets
  // its block allocated by that first message).
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
          " the history moved under it", (unsigned)after.seqNum, after.message,
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
    CHECK(espnowHistoryMessageCount(mac) == 0, "unknown peer has a count");
    espnowUpdateSentDeliveryState(mac, 7, 1);
  }
  CHECK(getAllMessages(out, 4, 0) == 1 && espnowCollapsedAllMessages(refs, 8) == 1, "all-peer reads");
  CHECK(gEspNow->peerHistoryCount == slots0,
        "reads for 20 unknown MACs created %d peer history slot(s) (read API creates slots)",
        gEspNow->peerHistoryCount - slots0);
  CHECK(gAllocCalls.load() == allocs0,
        "reads allocated %d time(s) (read API grows the history)", gAllocCalls.load() - allocs0);
  checkLockDiscipline();
}

static void caseBounded() {
  const size_t mark0 = lineMark();
  storeInit();
  CHECK(allocsOf("espnow.msgHist") == 0 && gAllocCalls.load() == 0, "init allocated");
  CHECK(gEspNow->peerHistoryCapacity == gMeshPeerSlots, "capacity %d, expected %d",
        gEspNow->peerHistoryCapacity, gMeshPeerSlots);

  uint8_t macs[17][6];
  for (int i = 0; i < 17; i++) macFor(10 + i, macs[i]);
  // Peers 0..7 within one minute: one allocation line each (at most one per slot per boot).
  for (int i = 0; i < 8; i++) CHECK(storeText(macs[i], "p", "a"), "store %d", i);
  CHECK(allocsOf("espnow.msgHist") == 8 && allocsOf("espnow.msgHistScratch") == 1,
        "8 peers: %d block and %d scratch allocation(s)", allocsOf("espnow.msgHist"),
        allocsOf("espnow.msgHistScratch"));
  CHECK(bytesOf("espnow.msgHist") == 8 * blockBytes() && blockBytes() == 92 + 2 * 16384,
        "block bytes %zu (block %zu)", bytesOf("espnow.msgHist"), blockBytes());
  CHECK(gAllocPolicy["espnow.msgHist"] == AllocPref::RequirePsram &&
        gAllocPolicy["espnow.msgHistScratch"] == AllocPref::RequirePsram, "PSRAM blocks not RequirePsram");
  std::string line;
  CHECK(linesWith(mark0, "Message history allocated for", &line) == 8, "%d allocation line(s) for 8 peers",
        linesWith(mark0, "Message history allocated for"));
  for (int i = 0; i < 8; i++) {
    char want[64];
    std::snprintf(want, sizeof(want), ": 33 KB (%d/16 peers", i + 1);
    CHECK(linesWith(mark0, (macText(macs[i]) + want).c_str()) == 1, "no allocation line for peer %d ('%s')", i,
          line.c_str());
  }
  CHECK(line.find("more since the last notice") == std::string::npos, "allocation line folded '%s'", line.c_str());
  size_t mark = lineMark();
  CHECK(storeText(macs[8], "p", "a"), "store 8");
  CHECK(linesWith(mark, "Message history allocated for", &line) == 1 && line.find("(9/16 peers") != std::string::npos,
        "allocation line '%s'", line.c_str());
  mark = lineMark();
  CHECK(storeText(macs[8], "p", "b"), "store 8 again");
  CHECK(linesWith(mark, "Message history allocated for") == 0, "a store into an existing block said line D");
  for (int i = 9; i < 16; i++) CHECK(storeText(macs[i], "p", "a"), "store %d", i);
  CHECK(gEspNow->peerHistoryCount == 16, "%d blocks, expected 16", gEspNow->peerHistoryCount);
  const size_t bound = 16 * sizeof(PeerMessageHistory) + std::max<size_t>(1024 * 1024, 16 * 2 * 1024) +
                       sizeof(PeerHistoryScratch);
  CHECK(bytesOf("espnow.msgHist") + bytesOf("espnow.msgHistScratch") <= bound, "16 peers use %zu bytes > %zu",
        bytesOf("espnow.msgHist") + bytesOf("espnow.msgHistScratch"), bound);
  const int allocsFull = gAllocCalls.load();

  PeerMessageHistory* blocks[16];
  for (int i = 0; i < 16; i++) blocks[i] = gEspNow->peerMessageHistories[i];

  // The 17th peer: refused, line B once a minute with the folded count.
  mark = lineMark();
  CHECK(!storeText(macs[16], "p", "a"), "17th peer stored past the slot limit");
  CHECK(linesWith(mark, "Message history full: 16 peers have one; " , &line) == 1 &&
        line.find(macText(macs[16]) + " not kept (1 message(s)") != std::string::npos, "line B '%s'", line.c_str());
  CHECK(!storeText(macs[16], "p", "a"), "17th peer stored on the second try");
  CHECK(linesWith(mark, "Message history full") == 1, "line B said twice within a minute");
  advanceMs(PEER_HISTORY_SAY_INTERVAL_MS + 1000);
  CHECK(!storeText(macs[16], "p", "a"), "17th peer stored on the third try");
  CHECK(linesWith(mark, "Message history full", &line) == 2 && line.find("(2 message(s)") != std::string::npos,
        "line B after a minute '%s'", line.c_str());
  CHECK(gAllocCalls.load() == allocsFull, "17th peer allocated");
  CHECK(gEspNow->peerHistoryCount == 16, "17th peer took a slot");

  CollapsedMsgRef refs[8];
  int n = espnowGetConversation(macs[0], refs, 8);
  CHECK(n == 1, "peer 0 conversation has %d messages", n);
  for (int i = 0; i < 1000; i++) storeText(macs[i % 16], "p", "b");
  for (int i = 0; i < 16; i++) {
    CHECK(gEspNow->peerMessageHistories[i] == blocks[i], "slot %d block moved after 1000 stores", i);
  }
  ReceivedTextMessage copy;
  CHECK(espnowCopyCollapsedHead(refs[0], &copy) && std::strcmp(copy.message, "a") == 0,
        "ref refused before its record was dropped");
  // Corrupted offsets still resolve by seq (walk), never by trusting the offset.
  CollapsedMsgRef bad = refs[0];
  for (uint32_t off : {3u, 8u, 0xFFFFFFFCu, 16380u, 20u}) {
    bad.offset = off;
    CHECK(espnowCopyCollapsedHead(bad, &copy) && copy.seqNum == refs[0].seqNum && std::strcmp(copy.message, "a") == 0,
          "ref with offset %u did not resolve by seq", (unsigned)off);
  }
  // Peer 0's own stores evict its oldest records; the ref is refused after.
  for (int i = 0; i < 1000; i++) storeText(macs[0], "p", "c");
  CHECK(!espnowCopyCollapsedHead(refs[0], &copy), "a ref whose record was dropped was accepted");
  bad = refs[0];
  bad.slot = 200;
  CHECK(!espnowCopyCollapsedHead(bad, &copy), "ref with an out-of-range slot accepted");
  bad.slot = 255;
  CHECK(!espnowCopyCollapsedHead(bad, &copy), "ref with slot 255 accepted");
  bad = refs[0];
  bad.isSent = true;
  CHECK(!espnowCopyCollapsedHead(bad, &copy), "ref resolved in the other ring");
  n = espnowGetConversation(macs[0], refs, 8);
  CHECK(n == 8 && espnowCopyCollapsedHead(refs[0], &copy), "fresh refs refused after the wrap");
  checkLockDiscipline();
}

static void caseViewClamp() {
  // A view asked for more than ESPNOW_HISTORY_VIEW_MAX entries gets the newest
  // ESPNOW_HISTORY_VIEW_MAX (the chain scratch size), not an empty view.
  storeInit();
  uint8_t A[6], B[6];
  macFor(1, A); macFor(2, B);
  char text[16];
  for (int i = 0; i < ESPNOW_HISTORY_VIEW_MAX + 40; i++) {
    std::snprintf(text, sizeof(text), "m%d", i);
    CHECK(storeText(i % 3 ? A : B, "p", text), "store %d", i);
  }
  const int big = ESPNOW_HISTORY_VIEW_MAX + 100;
  std::vector<CollapsedMsgRef> refs((size_t)big);
  ReceivedTextMessage copy;
  int n = espnowCollapsedAllMessages(refs.data(), big);
  std::snprintf(text, sizeof(text), "m%d", ESPNOW_HISTORY_VIEW_MAX + 39);
  CHECK(n == ESPNOW_HISTORY_VIEW_MAX && espnowCopyCollapsedHead(refs[(size_t)n - 1], &copy) &&
        std::strcmp(copy.message, text) == 0, "all-peers view with maxN %d: %d entries", big, n);
  const int aCount = espnowHistoryMessageCount(A);
  n = espnowGetConversation(A, refs.data(), big);
  CHECK(n == std::min(aCount, ESPNOW_HISTORY_VIEW_MAX) && n > 0, "conversation with maxN %d: %d entries (A has %d)",
        big, n, aCount);
  n = espnowCollapsedPeerMessages(A, refs.data(), big);
  CHECK(n == std::min(aCount, ESPNOW_HISTORY_VIEW_MAX) && n > 0, "peer view with maxN %d: %d entries", big, n);
  checkLockDiscipline();
}

static void caseAlloc() {
  // Failure, hold-off and the rate-limited line A. The clock is far from 0, so
  // the hold-off can only come from the time of the failure.
  advanceMs(100u * PEER_HISTORY_ALLOC_RETRY_MS);
  storeInit();
  uint8_t A[6], B[6], C[6], D[6];
  macFor(1, A); macFor(2, B); macFor(3, C); macFor(4, D);
  CHECK(storeText(A, "alpha", "one"), "first store");
  gAllocFail = true;
  size_t mark = lineMark();
  const uint32_t seq0 = gEspNow->globalMessageSeqNum;
  int calls = gAllocCalls.load();
  CHECK(!storeText(B, "bravo", "x"), "store without memory succeeded");
  std::string line;
  CHECK(linesWith(mark, "not kept: no memory for 33 KB in PSRAM", &line) == 1 &&
        line.find(macText(B)) != std::string::npos && line.find("(1 message(s) not kept") != std::string::npos,
        "line A '%s'", line.c_str());
  CHECK(gAllocCalls.load() == calls + 1, "failed store called ps_alloc %d time(s)", gAllocCalls.load() - calls);
  CHECK(gEspNow->peerHistoryCount == 1 && gEspNow->globalMessageSeqNum == seq0, "failed store changed the store");
  calls = gAllocCalls.load();
  gAllocFail = false;
  CHECK(!storeText(B, "bravo", "y"), "store within the hold-off succeeded");
  CHECK(gAllocCalls.load() == calls, "store within the hold-off called ps_alloc");
  CHECK(linesWith(mark, "not kept: no memory") == 1, "line A said twice within a minute");
  CHECK(storeText(A, "alpha", "two"), "a peer with a block is not held off");
  advanceMs(PEER_HISTORY_ALLOC_RETRY_MS + 100);
  CHECK(storeText(B, "bravo", "z"), "retry after the hold-off failed");
  CHECK(gEspNow->peerHistoryCount == 2, "retry did not activate a slot");
  ReceivedTextMessage out[4];
  CHECK(getPeerMessages(B, out, 4, 0) == 1 && std::strcmp(out[0].message, "z") == 0, "retried peer's records");
  gAllocFail = true;
  CHECK(!storeText(C, "c", "x"), "store without memory succeeded");
  CHECK(linesWith(mark, "not kept: no memory") == 1, "line A not rate-limited");
  advanceMs(PEER_HISTORY_SAY_INTERVAL_MS + 1000);
  CHECK(!storeText(C, "c", "x"), "store without memory succeeded");
  CHECK(linesWith(mark, "not kept: no memory", &line) == 2 && line.find("(3 message(s) not kept") != std::string::npos,
        "line A after a minute '%s'", line.c_str());
  gAllocFail = false;

  // The scratch comes with the first block: if it fails, the block is freed.
  advanceMs(PEER_HISTORY_ALLOC_RETRY_MS + 100);
  CHECK(storeText(C, "c", "ok"), "store after the failures");
  checkLockDiscipline();
}

static void caseAllocScratch() {
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  gAllocFailTag = "espnow.msgHistScratch";
  const int frees0 = gFreeCalls.load();
  CHECK(!storeText(A, "alpha", "x"), "store without a scratch succeeded");
  CHECK(gFreeCalls.load() == frees0 + 1 && gEspNow->peerHistoryCount == 0 && gEspNow->peerMessageHistories[0] == nullptr,
        "the block of a refused first store was kept");
  gAllocFailTag.clear();
  advanceMs(PEER_HISTORY_ALLOC_RETRY_MS + 100);
  CHECK(storeText(A, "alpha", "y"), "retry failed");
  {
    std::lock_guard<std::mutex> lock(gAllocMutex);
    CHECK(gLive.size() == 2, "%zu live allocations, expected block + scratch", gLive.size());
  }
  checkLockDiscipline();
}

static void caseAllocInternal() {
  // No PSRAM at runtime on a CONFIG_SPIRAM build: the no-PSRAM budget and
  // RequireInternal, refused while the internal floor would be crossed.
  gPsram = false;
  size_t mark = lineMark();
  storeInit();
  std::string line;
  CHECK(gEspNow->peerHistoryRingBytes == 2048, "no-PSRAM ring %u", (unsigned)gEspNow->peerHistoryRingBytes);
  CHECK(linesWith(mark, "[ESP-NOW] Message history: 4 KB per peer (2 KB received + 2 KB sent) in internal RAM", &line) == 1,
        "start line '%s'", lastBroadcast().c_str());
  uint8_t A[6];
  macFor(1, A);
  gFreeInternal = 48 * 1024 + 1000;  // below floor + block + scratch
  // IRAM-only free memory (classic ESP32) cannot hold the block and must not
  // count toward the floor (review 2026-11-18).
  gFreeIramOnly = 64 * 1024;
  CHECK(!storeText(A, "alpha", "x"), "store under the internal floor succeeded (IRAM-only memory counted)");
  CHECK(gAllocCalls.load() == 0, "ps_alloc called under the internal floor");
  CHECK(linesWith(mark, "no memory for 5 KB in internal RAM") == 1, "line A '%s'", lastBroadcast().c_str());
  gFreeInternal = 200 * 1024;
  gLargestInternal = 1000;  // free heap, but no block that large
  gLargestIramOnly = 60 * 1024;  // only in IRAM, which the block cannot use
  advanceMs(PEER_HISTORY_ALLOC_RETRY_MS + 100);
  CHECK(!storeText(A, "alpha", "x"), "store without a large enough block succeeded (IRAM-only block counted)");
  CHECK(gAllocCalls.load() == 0, "ps_alloc called without a large enough block");
  gLargestInternal = 200 * 1024;
  advanceMs(PEER_HISTORY_ALLOC_RETRY_MS + 100);
  CHECK(storeText(A, "alpha", "y"), "store with room failed");
  CHECK(gAllocPolicy["espnow.msgHist"] == AllocPref::RequireInternal &&
        gAllocPolicy["espnow.msgHistScratch"] == AllocPref::RequireInternal, "internal blocks not RequireInternal");
  CHECK(bytesOf("espnow.msgHist") == 92 + 2 * 2048, "internal block %zu bytes", bytesOf("espnow.msgHist"));
  checkLockDiscipline();
}

static void caseBudget() {
  // The total cap (a quarter of the PSRAM found) lowers the ring, and a
  // setting above the board maximum is clamped; the start line says both.
  gTotalSpiram = 2u * 1024u * 1024u;  // quarter = 512 KB -> 16 KB per ring over 16 slots
  size_t mark = lineMark();
  storeInit(40);
  std::string line;
  CHECK(gEspNow->peerHistoryRingBytes == 16384, "ring %u", (unsigned)gEspNow->peerHistoryRingBytes);
  CHECK(linesWith(mark, "Message history: ", &line) == 1 &&
        line.find(" (espnowhistorykb 40 is above this board's 32) (lowered to fit the 512 KB history cap)") !=
            std::string::npos, "start line '%s'", line.c_str());
}

static void caseTimeOrder() {
  // Pieces out of order: piece 2 opens the group, a sent message follows, then
  // piece 1 becomes the head with the latest timestamp. The conversation and
  // the all-peers view are ordered by the head's time (as before), so the sent
  // message comes first; the order of opening alone would put the group first.
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  CHECK(storeMessageInPeerHistory(A, "alpha", "two", true, MSG_TEXT, 9, 2, 2), "piece 2");
  advanceMs(10);
  CHECK(storeSentMessageInPeerHistory(A, "reply", 50), "sent");
  advanceMs(10);
  CHECK(storeMessageInPeerHistory(A, "alpha", "one", true, MSG_TEXT, 9, 1, 2), "piece 1");
  CollapsedMsgRef refs[8];
  for (int view = 0; view < 2; view++) {
    const int n = view == 0 ? espnowGetConversation(A, refs, 8) : espnowCollapsedAllMessages(refs, 8);
    CHECK(n == 2 && refs[0].isSent && refs[0].reqId == 50 && !refs[1].isSent && refs[1].reqId == 9 &&
              refs[1].headPiece == 1 && refs[1].partsPresent == 2 && refs[0].timestamp <= refs[1].timestamp,
          "view %d: %d entries, first sent %d reqId %u", view, n, n > 0 ? (int)refs[0].isSent : -1,
          n > 0 ? (unsigned)refs[0].reqId : 0u);
  }
  checkLockDiscipline();
}

static void caseBudgetBytes() {
  // No PSRAM, 12 mesh slots, espnowhistorykb 4: the 64 KB cap lowers each ring
  // to floor4(65536 / 24) = 2728 bytes, not whole KB; the lines say bytes.
  gPsram = false;
  gMeshPeerSlots = 12;
  size_t mark = lineMark();
  storeInit(4);
  std::string line;
  CHECK(gEspNow->peerHistoryRingBytes == 2728 && gEspNow->peerHistoryCapacity == 12, "ring %u capacity %d",
        (unsigned)gEspNow->peerHistoryRingBytes, gEspNow->peerHistoryCapacity);
  CHECK(linesWith(mark, "Message history: ", &line) == 1 &&
        line.find("Message history: 5456 bytes per peer (2728 bytes received + 2728 bytes sent) in internal RAM") !=
            std::string::npos &&
        line.find("up to 12 peers") != std::string::npos &&
        line.find("(lowered to fit the 64 KB history cap)") != std::string::npos,
        "start line '%s'", line.c_str());
  char status[256];
  espnowHistoryStatusLine(status, sizeof(status));
  CHECK(std::strstr(status, "saved 0 KB (0 = auto), active 2728 bytes per ring, 0/12 peers") != nullptr,
        "status '%s'", status);
  uint8_t A[6];
  macFor(1, A);
  CHECK(storeText(A, "alpha", "x"), "store");
  CHECK(bytesOf("espnow.msgHist") == 92 + 2 * 2728, "block %zu bytes", bytesOf("espnow.msgHist"));
}

static void caseSlotsClamp() {
  // More mesh slots than the slot table: the capacity is MESH_PEER_MAX.
  gMeshPeerSlots = MESH_PEER_MAX + 24;
  storeInit();
  CHECK(gEspNow->peerHistoryCapacity == MESH_PEER_MAX, "capacity %d", gEspNow->peerHistoryCapacity);
  for (int i = 0; i < MESH_PEER_MAX + 2; i++) {
    uint8_t mac[6];
    macFor(40 + i, mac);
    CHECK(storeText(mac, "p", "a") == (i < MESH_PEER_MAX), "store for peer %d", i);
  }
  CHECK(gEspNow->peerHistoryCount == MESH_PEER_MAX, "%d blocks", gEspNow->peerHistoryCount);
  checkLockDiscipline();
}

static void caseInstallRace() {
  // Another task installs a block for the same MAC between our unlock and
  // re-lock: ours is freed after unlock and the record lands in the first.
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  bool hookRan = false;
  gAllocHook = [&] {
    hookRan = true;
    CHECK(storeText(A, "alpha", "first"), "inner store failed");
  };
  const int frees0 = gFreeCalls.load();
  CHECK(storeText(A, "alpha", "second"), "outer store failed");
  CHECK(hookRan, "race hook did not run");
  CHECK(gEspNow->peerHistoryCount == 1, "%d blocks for one MAC", gEspNow->peerHistoryCount);
  CHECK(gFreeCalls.load() == frees0 + 2, "%d free(s): the losing block and scratch must be freed",
        gFreeCalls.load() - frees0);
  ReceivedTextMessage out[4];
  int n = getPeerMessages(A, out, 4, 0);
  CHECK(n == 2 && std::strcmp(out[0].message, "first") == 0 && std::strcmp(out[1].message, "second") == 0,
        "records after the install race: %d", n);
  {
    std::lock_guard<std::mutex> lock(gAllocMutex);
    CHECK(gLive.size() == 2, "%zu live allocations after the race", gLive.size());
  }
  checkLockDiscipline();
}

static void caseInstallFull() {
  // Between our allocation (outside the lock) and the install, other stores
  // take every free slot: ours is refused with line B ("full"), nothing of it
  // is kept, and its block and scratch are freed after unlock.
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  bool hookRan = false;
  gAllocHook = [&] {
    hookRan = true;
    for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
      uint8_t mac[6];
      macFor(100 + i, mac);
      CHECK(storeText(mac, "p", "x"), "inner store %d failed", i);
    }
  };
  const size_t mark = lineMark();
  const int frees0 = gFreeCalls.load();
  CHECK(!storeText(A, "alpha", "late"), "store after the slots filled up succeeded");
  CHECK(hookRan, "fill hook did not run");
  CHECK(gEspNow->peerHistoryCount == gEspNow->peerHistoryCapacity && blockOf(A) == nullptr,
        "%d blocks, A has one: %d", gEspNow->peerHistoryCount, blockOf(A) != nullptr);
  CHECK(gFreeCalls.load() == frees0 + 2, "%d free(s): our block and scratch must be freed", gFreeCalls.load() - frees0);
  std::string line;
  CHECK(linesWith(mark, "Message history full", &line) == 1 && line.find(macText(A) + " not kept") != std::string::npos,
        "line B '%s'", line.c_str());
  {
    std::lock_guard<std::mutex> lock(gAllocMutex);
    CHECK(gLive.size() == (size_t)gEspNow->peerHistoryCapacity + 1, "%zu live allocations", gLive.size());
  }
  checkLockDiscipline();
}

static void caseSeqWrap() {
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  CHECK(storeText(A, "alpha", "x"), "first store");
  gEspNow->globalMessageSeqNum = 0xFFFFFFF0u;
  for (int i = 0; i < 15; i++) CHECK(storeText(A, "alpha", "w"), "store %d below the seq limit", i);
  CHECK(gEspNow->globalMessageSeqNum == UINT32_MAX, "seq %u", (unsigned)gEspNow->globalMessageSeqNum);
  size_t mark = lineMark();
  CHECK(!storeText(A, "alpha", "over"), "store past the seq limit succeeded");
  CHECK(!storeSentMessageInPeerHistory(A, "over", 9), "sent store past the seq limit succeeded");
  std::string line;
  CHECK(linesWith(mark, "message numbers used up", &line) == 1 && line.find("1 message(s)") != std::string::npos,
        "line E '%s'", line.c_str());
  CHECK(gEspNow->globalMessageSeqNum == UINT32_MAX, "seq wrapped to %u", (unsigned)gEspNow->globalMessageSeqNum);
  advanceMs(PEER_HISTORY_SAY_INTERVAL_MS + 1000);
  CHECK(!storeText(A, "alpha", "over"), "store past the seq limit succeeded");
  CHECK(linesWith(mark, "message numbers used up", &line) == 2 && line.find("2 message(s)") != std::string::npos,
        "line E after a minute '%s'", line.c_str());
  ReceivedTextMessage out[32];
  int n = getPeerMessages(A, out, 32, 0);
  CHECK(n == 16 && out[15].seqNum == UINT32_MAX, "readers after the limit: %d records", n);
  checkLockDiscipline();
}

static std::vector<uint8_t> snapshot(const PeerMessageHistory* h) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(h);
  return std::vector<uint8_t>(p, p + sizeof(PeerMessageHistory) + 2u * h->ringBytes);
}

static void caseIsolation() {
  storeInit();
  uint8_t macs[4][6];
  for (int i = 0; i < 4; i++) macFor(40 + i, macs[i]);
  for (int i = 0; i < 4; i++) {
    for (int k = 0; k < 50; k++) {
      storeText(macs[i], "peer", patternText(10 + k, (char)k).c_str());
      storeSentMessageInPeerHistory(macs[i], "reply", 1000 + k);
    }
  }
  std::vector<uint8_t> before[4];
  for (int i = 1; i < 4; i++) before[i] = snapshot(blockOf(macs[i]));
  const std::string longText = patternText(2000, 5);
  for (int k = 0; k < 2000; k++) {
    switch (k % 4) {
      case 0: storeText(macs[0], "peer", patternText((size_t)(k % 255), (char)k).c_str()); break;
      case 1: storeReceivedMessageChunked(macs[0], "peer", longText.c_str(), true, MSG_CMD_RESULT, 5000 + k); break;
      case 2: storeSentMessageInPeerHistory(macs[0], longText.c_str(), 9000 + k); break;
      default: espnowUpdateSentDeliveryState(macs[0], 9000 + k - 1, (uint8_t)(k % 4)); break;
    }
  }
  PeerMessageHistory* a = blockOf(macs[0]);
  CHECK(a && a->received.evicted > 0 && a->sent.evicted > 0, "peer A's rings did not wrap");
  for (int i = 1; i < 4; i++) {
    CHECK(snapshot(blockOf(macs[i])) == before[i], "peer %d's block changed while peer A stored", i);
  }
  checkLockDiscipline();
}

// Offset of the k-th record (oldest first) of a ring, by the validated walk.
static uint32_t recordOffset(PeerMessageHistory* h, bool sent, int k) {
  const espnowhist::RingState& r = sent ? h->sent : h->received;
  const uint8_t* buf = sent ? h->sentBuf() : h->receivedBuf();
  espnowhist::Cursor c = espnowhist::begin(r);
  uint32_t off = 0;
  espnowhist::Header hd{};
  for (int i = 0; i <= k; i++) {
    if (!espnowhist::next(r, buf, c, off, hd)) return UINT32_MAX;
  }
  return off;
}

static void caseDamaged() {
  storeInit();
  uint8_t A[6], B[6];
  macFor(1, A); macFor(2, B);
  char text[16];
  for (int i = 0; i < 10; i++) {
    std::snprintf(text, sizeof(text), "r%d", i);
    storeText(A, "alpha", text);
    storeText(B, "bravo", text);
  }
  storeSentMessageInPeerHistory(A, "sent", 77);
  PeerMessageHistory* a = blockOf(A);
  const std::vector<uint8_t> bBefore = snapshot(blockOf(B));
  const uint32_t off = recordOffset(a, false, 5);
  CHECK(off != UINT32_MAX, "no record 5");
  a->receivedBuf()[off + espnowhist::kOffType] ^= 0x40;  // one header byte

  size_t mark = lineMark();
  ReceivedTextMessage out[32];
  int n = getPeerMessages(A, out, 32, 0);
  int received = 0;
  bool ok = true;
  for (int i = 0; i < n; i++) {
    ok = ok && viewOk(out[i]);
    if (!out[i].isSent) {
      std::snprintf(text, sizeof(text), "r%d", received);
      ok = ok && std::strcmp(out[i].message, text) == 0;
      received++;
    }
  }
  CHECK(ok && received == 5, "damaged read returned %d received records before the damage", received);
  std::string line;
  CHECK(linesWith(mark, "was damaged and was cleared", &line) == 1 && line.find(macText(A)) != std::string::npos,
        "line C '%s'", line.c_str());
  CHECK(a->received.count == 0 && a->received.resets == 1 && a->sent.count == 1, "damaged ring not cleared");
  CHECK(storeText(A, "alpha", "after"), "store after the damage");
  n = getPeerMessages(A, out, 32, 0);
  CHECK(n == 2 && std::strcmp(out[1].message, "after") == 0, "store after the damage not visible (%d)", n);

  // A broken RingState field: caught by the next op's check.
  a->received.head = 3;
  CHECK(espnowHistoryMessageCount(A) == 0 && a->received.resets == 2, "broken head not caught");
  CHECK(linesWith(mark, "was damaged and was cleared") == 1, "line C said twice within a minute");
  // A broken ringBytes: the size is put back and the ring cleared; the sent
  // ring is right again and keeps its record.
  storeText(A, "alpha", "again");
  a->ringBytes = 1028;
  n = getPeerMessages(A, out, 32, 0);
  CHECK(a->ringBytes == 16384 && n == 1 && out[0].isSent && std::strcmp(out[0].message, "sent") == 0,
        "broken ringBytes: %d records, ringBytes %u", n, (unsigned)a->ringBytes);
  // A ring state that is sane but the wrong size for this boot (an empty
  // 8 KB ring): the store's check puts it back to the latched size before
  // writing, so the record stays readable (review 2026-11-18, mutant R10).
  espnowhist::ringInit(a->received, 8192, false);
  CHECK(storeText(A, "alpha", "resized"), "store over a wrong-size ring");
  CHECK(a->received.size == 16384 && a->received.count == 1, "store wrote into a wrong-size ring (size %u count %u)",
        (unsigned)a->received.size, (unsigned)a->received.count);
  n = getPeerMessages(A, out, 32, 0);
  CHECK(n == 2 && std::strcmp(out[1].message, "resized") == 0, "record stored over a wrong-size ring lost (%d)", n);
  // ringBytes and both ring sizes moved together (8 KB, each still sane): the
  // block's size is checked against the latched one, so sentBuf() is never
  // taken at the wrong place; both rings are cleared (mutant I01).
  a->ringBytes = 8192;
  espnowhist::ringInit(a->sent, 8192, true);
  a->received.size = 8192;
  a->received.wrapAt = 8192;
  CHECK(espnowhist::stateSane(a->received) && espnowhist::stateSane(a->sent), "coordinated state not sane");
  n = getPeerMessages(A, out, 32, 0);
  CHECK(n == 0 && a->ringBytes == 16384 && a->received.size == 16384 && a->sent.size == 16384 &&
        a->received.count == 0, "coordinated size damage: %d records, ringBytes %u", n, (unsigned)a->ringBytes);
  // Damage met by a delivery stamp: the sent ring is cleared (mutant I19).
  CHECK(storeSentMessageInPeerHistory(A, "s1", 78) && storeSentMessageInPeerHistory(A, "s2", 79), "sent stores");
  const uint32_t sentResets = a->sent.resets;
  a->sentBuf()[recordOffset(a, true, 1) + espnowhist::kOffCheck] ^= 1;
  espnowUpdateSentDeliveryState(A, 79, 1);
  CHECK(a->sent.count == 0 && a->sent.resets == sentResets + 1, "damage met by a stamp not cleared (count %u)",
        (unsigned)a->sent.count);
  CHECK(storeSentMessageInPeerHistory(A, "sent", 77), "sent store after the stamp damage");
  // Damage met by a collapse and by a head copy.
  for (int i = 0; i < 6; i++) storeText(A, "alpha", "c");
  a->receivedBuf()[recordOffset(a, false, 3) + espnowhist::kOffCheck] ^= 1;
  CollapsedMsgRef refs[16];
  n = espnowGetConversation(A, refs, 16);
  CHECK(n == 4 && a->received.count == 0, "collapse over damage: %d entries, count %d", n, (int)a->received.count);
  storeText(A, "alpha", "d0");
  storeText(A, "alpha", "d1");
  n = espnowCollapsedPeerMessages(A, refs, 16);
  CHECK(n == 2, "collapse after reset: %d", n);
  a->receivedBuf()[recordOffset(a, false, 0) + espnowhist::kOffSeq] ^= 1;
  ReceivedTextMessage copy;
  CHECK(!espnowCopyCollapsedHead(refs[0], &copy) && a->received.count == 0, "head copy over damage");
  CHECK(snapshot(blockOf(B)) == bBefore, "peer B changed while peer A was damaged");
  checkLockDiscipline();
}

static void caseNames() {
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  const std::string mac = macText(A);
  struct Row { const char* name; HistoryAttribution attr; bool unattributed; };
  const Row rows[] = {
    {"alpha", HistoryAttribution::FromName, false},
    {nullptr, HistoryAttribution::FromName, true},
    {"", HistoryAttribution::FromName, true},
    {mac.c_str(), HistoryAttribution::FromName, true},
    {mac.c_str(), HistoryAttribution::Attributed, false},
    {"alpha", HistoryAttribution::Unattributed, true},
    {nullptr, HistoryAttribution::Attributed, false},
  };
  const int kRows = (int)(sizeof(rows) / sizeof(rows[0]));
  for (int i = 0; i < kRows; i++) {
    CHECK(storeMessageInPeerHistory(A, rows[i].name, "t", false, MSG_TEXT, 0, 1, 1, rows[i].attr), "store row %d", i);
  }
  CHECK(storeReceivedMessageChunked(A, "beta", "chunk", false, MSG_CMD_RESULT, 3, HistoryAttribution::Unattributed),
        "chunked unattributed");
  CHECK(storeReceivedMessageChunked(A, mac.c_str(), "chunk", false, MSG_CMD_RESULT, 4), "chunked FromName MAC");
  CHECK(storeSentMessageInPeerHistory(A, "sent", 5), "sent");
  ReceivedTextMessage out[16];
  int n = getPeerMessages(A, out, 16, 0);
  CHECK(n == kRows + 3, "%d records", n);
  for (int i = 0; i < kRows && i < n; i++) {
    CHECK(out[i].unattributed == rows[i].unattributed, "row %d unattributed=%d", i, (int)out[i].unattributed);
  }
  if (n == kRows + 3) {
    CHECK(out[kRows].unattributed && out[kRows + 1].unattributed && !out[kRows + 2].unattributed &&
          out[kRows + 2].isSent, "chunked/sent attribution");
  }
  checkLockDiscipline();
}

static void caseIntake() {
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  // A 300-byte buffer with no NUL: the store reads at most 256 bytes.
  char* raw = static_cast<char*>(std::malloc(300));
  std::memset(raw, 'q', 300);
  CHECK(storeMessageInPeerHistory(A, "alpha", raw, false, MSG_TEXT), "unterminated store");
  std::free(raw);
  ReceivedTextMessage out[300];
  int n = getPeerMessages(A, out, 300, 0);
  CHECK(n == 1 && out[0].length == 255 && std::strlen(out[0].message) == 255, "unterminated record %d bytes",
        n ? (int)out[0].length : -1);
  // 52,000 bytes chunked: 255 pieces of 200, pieceTotal 255, the rest dropped.
  std::string huge = patternText(52000, 1);
  const uint32_t seq0 = gEspNow->globalMessageSeqNum;
  CHECK(storeReceivedMessageChunked(A, "alpha", huge.c_str(), true, MSG_CMD_RESULT, 99), "huge chunked store");
  CHECK(gEspNow->globalMessageSeqNum - seq0 == 255, "%u pieces stored", (unsigned)(gEspNow->globalMessageSeqNum - seq0));
  n = getPeerMessages(A, out, 300, 0);
  bool ok = n > 0 && out[n - 1].piece == 255 && out[n - 1].length == 200;
  for (int i = 0; i < n; i++) {
    if (out[i].reqId == 99) ok = ok && out[i].pieceTotal == 255 && out[i].length == 200;
  }
  CHECK(ok, "huge chunked store kept %d records, newest piece %u", n, n ? out[n - 1].piece : 0);
  checkLockDiscipline();
}

static void casePaging() {
  storeInit();
  uint8_t macs[3][6];
  for (int i = 0; i < 3; i++) macFor(60 + i, macs[i]);
  std::vector<uint32_t> all;
  for (int k = 0; k < 90; k++) {
    const int p = (k * 7) % 3;  // interleaved
    if (k % 5 == 0) storeSentMessageInPeerHistory(macs[p], "s", 100 + k);
    else storeText(macs[p], "peer", "r");
    all.push_back(gEspNow->globalMessageSeqNum);
  }
  ReceivedTextMessage out[5];
  int n = getAllMessages(out, 5, 30);
  CHECK(n == 5, "first page %d", n);
  for (int i = 0; i < n; i++) CHECK(out[i].seqNum == 31u + (uint32_t)i, "page entry %d seq %u", i, (unsigned)out[i].seqNum);
  uint32_t since = 0;
  std::vector<uint32_t> seen;
  for (int round = 0; round < 100; round++) {
    n = getAllMessages(out, 5, since);
    if (n == 0) break;
    for (int i = 0; i < n; i++) {
      seen.push_back(out[i].seqNum);
      CHECK(out[i].seqNum > since, "page returned seq %u at or below since %u", (unsigned)out[i].seqNum,
            (unsigned)since);
    }
    since = out[n - 1].seqNum;
  }
  CHECK(seen == all, "paging visited %zu records, expected %zu in order", seen.size(), all.size());
  const int decodes0 = (int)espnowhist::decodeCount();
  CHECK(getAllMessages(out, 5, since) == 0 && (int)espnowhist::decodeCount() == decodes0,
        "an idle poll decoded %d record(s)", (int)espnowhist::decodeCount() - decodes0);
  checkLockDiscipline();
}

// A received remote command row carries its outcome in the record's three
// state bits (ESP-NOW 1.0 step 6): busy (1) on a one-piece row, late (5) on
// every piece of a result; a plain result row stays 0.
static void caseOutcomeRows() {
  storeInit();
  uint8_t A[6];
  macFor(2, A);
  const std::string longText = patternText(450, 4);
  CHECK(storeReceivedMessageChunked(A, "alpha", "Busy: alpha could not take the command now.", true,
                                    MSG_CMD_RESULT, 91, HistoryAttribution::Attributed, 1),
        "outcome row stored");
  CHECK(storeReceivedMessageChunked(A, "alpha", longText.c_str(), true, MSG_CMD_RESULT, 92,
                                    HistoryAttribution::Attributed, 5),
        "late result stored");
  CHECK(storeReceivedMessageChunked(A, "alpha", "ok", true, MSG_CMD_RESULT, 93), "plain result stored");
  ReceivedTextMessage rows[16];
  const int nr = getPeerMessages(A, rows, 16, 0);
  int busyRows = 0, lateRows = 0, plain = 0, other = 0;
  for (int i = 0; i < nr; i++) {
    if (rows[i].reqId == 91 && rows[i].sendState == 1 && !rows[i].isSent) ++busyRows;
    else if (rows[i].reqId == 92 && rows[i].sendState == 5 && !rows[i].isSent) ++lateRows;
    else if (rows[i].reqId == 93 && rows[i].sendState == 0) ++plain;
    else ++other;
  }
  CHECK(busyRows == 1 && lateRows == 3 && plain == 1 && other == 0,
        "outcomes read back on received rows (busy 1, late x3, plain 0)");
}

static void caseDelivery() {
  storeInit();
  uint8_t A[6];
  macFor(1, A);
  const std::string longText = patternText(450, 2);
  CHECK(storeSentMessageInPeerHistory(A, longText.c_str(), 42), "sent");
  CHECK(storeSentMessageInPeerHistory(A, "other", 43), "sent");
  const std::vector<uint8_t> before = snapshot(blockOf(A));
  // Three state bits since ESP-NOW 1.0 step 6 (busy 4, refused 5): above 7 is refused.
  for (int s = 8; s <= 255; s++) espnowUpdateSentDeliveryState(A, 42, (uint8_t)s);
  espnowUpdateSentDeliveryState(A, 0, 1);
  CHECK(snapshot(blockOf(A)) == before, "a state above 7 or msgId 0 changed the ring");
  espnowUpdateSentDeliveryState(A, 43, 4);
  {
    ReceivedTextMessage busy[8];
    const int nb = getPeerMessages(A, busy, 8, 0);
    bool stamped = false;
    for (int i = 0; i < nb; i++) stamped = stamped || (busy[i].reqId == 43 && busy[i].sendState == 4);
    CHECK(stamped, "a busy send (state 4) is stamped and read back");
  }
  // A refused send keeps its reason (the ACK status) with the record.
  espnowUpdateSentDeliveryState(A, 43, 5, 4);
  {
    ReceivedTextMessage rf[8];
    const int nf = getPeerMessages(A, rf, 8, 0);
    bool kept = false;
    for (int i = 0; i < nf; i++) kept = kept || (rf[i].reqId == 43 && rf[i].sendState == 5 && rf[i].sendStatus == 4);
    CHECK(kept, "a refused send (state 5) keeps its status 4 (too large)");
  }
  espnowUpdateSentDeliveryState(A, 43, 0);
  espnowUpdateSentDeliveryState(A, 42, 2);
  ReceivedTextMessage out[8];
  int n = getPeerMessages(A, out, 8, 0);
  int stamped = 0;
  for (int i = 0; i < n; i++) {
    if (out[i].reqId == 42) stamped += out[i].sendState == 2;
    else CHECK(out[i].sendState == 0, "msg 43 stamped");
  }
  CHECK(n == 4 && stamped == 3, "%d records, %d stamped", n, stamped);
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

static void caseRace() {
  storeInit();
  uint8_t macs[12][6];
  for (int i = 0; i < 12; i++) macFor(300 + i, macs[i]);
  const std::string longText = patternText(450, 1);
  std::atomic<bool> stop{false};
  std::atomic<int> writes{0}, reads{0}, badRecords{0}, staleCopies{0};
  std::atomic<uint32_t> maxSeqSeen{0};

  auto noteRecord = [&](const ReceivedTextMessage& r) {
    if (!viewOk(r)) badRecords++;
    uint32_t seen = maxSeqSeen.load();
    while (r.seqNum > seen && !maxSeqSeen.compare_exchange_weak(seen, r.seqNum)) {}
  };

  std::thread writer([&] {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    for (uint32_t i = 1; std::chrono::steady_clock::now() < end; i++) {
      uint8_t* mac = macs[i % 12];
      switch (i % 4) {
        case 0: storeText(mac, "racer", patternText(i % 200, (char)i).c_str()); break;
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
    CollapsedMsgRef* refs = new CollapsedMsgRef[ESPNOW_HISTORY_VIEW_MAX];
    std::vector<CollapsedMsgRef> old;  // refs from earlier rounds, stale by now
    ReceivedTextMessage copy;
    char buf[1200];
    for (int k = 0; !stop.load(); k++) {
      int n = getAllMessages(out, 128, 0);
      for (int i = 0; i < n; i++) noteRecord(out[i]);
      n = getPeerMessages(macs[k % 12], out, 128, 0);
      for (int i = 0; i < n; i++) noteRecord(out[i]);
      n = espnowGetConversation(macs[k % 12], refs, ESPNOW_HISTORY_VIEW_MAX);
      for (int i = 0; i < n; i += 7) {
        if (espnowCopyCollapsedHead(refs[i], &copy)) noteRecord(copy);
      }
      for (const CollapsedMsgRef& r : old) {
        if (espnowCopyCollapsedHead(r, &copy)) {
          noteRecord(copy);
          if (copy.seqNum != r.seqNum) badRecords++;
          staleCopies++;
        }
      }
      old.clear();
      n = espnowCollapsedAllMessages(refs, ESPNOW_HISTORY_VIEW_MAX);
      for (int i = 0; i < n; i += 11) {
        old.push_back(refs[i]);
        if (espnowCopyCollapsedHead(refs[i], &copy)) {
          noteRecord(copy);
          if (refs[i].reqId) {
            int len = espnowReassembleByReqId(copy.peerMac, refs[i].reqId, refs[i].isSent, buf, sizeof(buf));
            if (len < 0 || (size_t)len >= sizeof(buf) || std::strlen(buf) != (size_t)len) badRecords++;
          }
        }
      }
      int count = espnowHistoryMessageCount(nullptr);
      if (count < 0 || count > 12 * 16384 / 20) badRecords++;
      reads++;
    }
    delete[] out;
    delete[] refs;
  });

  writer.join();
  reader.join();
  std::printf("race: %d store calls, %d reader rounds, %d stale-ref copies\n", writes.load(), reads.load(),
              staleCopies.load());
  CHECK(writes.load() > 0 && reads.load() > 0, "race did not run (%d writes, %d reads)", writes.load(),
        reads.load());
  CHECK(badRecords.load() == 0, "%d torn or wrong record(s) read during concurrent stores", badRecords.load());
  CHECK(maxSeqSeen.load() <= gEspNow->globalMessageSeqNum, "read seq %u beyond the store's %u",
        (unsigned)maxSeqSeen.load(), (unsigned)gEspNow->globalMessageSeqNum);
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    PeerMessageHistory* h = gEspNow->peerMessageHistories[i];
    if (!h) continue;
    CHECK(espnowhist::checkRing(h->received, h->receivedBuf()).ok && espnowhist::checkRing(h->sent, h->sentBuf()).ok,
          "slot %d ring broken after the race", i);
  }
  checkLockDiscipline();
}

int main(int argc, char** argv) {
  buildState();
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return 2;
  }
  gCase = argv[1];
  const std::string which = argv[1];
  static const std::map<std::string, void (*)()> kCases = {
    {"ctor", caseCtor}, {"roundtrip", caseRoundTrip}, {"growth", caseGrowth}, {"find_only", caseFindOnly},
    {"bounded", caseBounded}, {"alloc", caseAlloc}, {"alloc_scratch", caseAllocScratch},
    {"alloc_internal", caseAllocInternal}, {"budget", caseBudget}, {"budget_bytes", caseBudgetBytes},
    {"slots_clamp", caseSlotsClamp}, {"view_clamp", caseViewClamp}, {"time_order", caseTimeOrder}, {"install_race", caseInstallRace}, {"install_full", caseInstallFull},
    {"seq_wrap", caseSeqWrap}, {"isolation", caseIsolation}, {"damaged", caseDamaged}, {"names", caseNames},
    {"intake", caseIntake}, {"paging", casePaging}, {"delivery", caseDelivery}, {"outcome_rows", caseOutcomeRows},
    {"lock_refusal", caseLockRefusal}, {"race", caseRace},
  };
  auto it = kCases.find(which);
  if (it == kCases.end()) {
    std::fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
  }
  it->second();
  if (gFailures.load()) return 1;
  std::printf("ok %s (%ld checks)\n", argv[1], gChecks.load());
  return 0;
}
