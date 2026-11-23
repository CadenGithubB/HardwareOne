// Host harness for the ESP-NOW file transfer (1.0 step 7, the Q26 layout):
// receive slot table, FILE_* handlers, sender and the commands that report a
// send.
//
// test_espnow_file_transfer.py splices in production code at the INSERT tags:
// the whole implementation of System_ESPNow_Files.cpp (with its result
// memory), every FILE_START / DATA / END / RESULT / CANCEL handler with the
// finalize jobs, the FILE_CANCEL and FILE_RESULT senders, sendFileToMacResult
// with its session gate, and espnowsendfile / espnowsend / imagesend / the
// OLED send job with the real CommandArgs. The wire structs, the receive
// context and the MAC text helpers are the real headers; the filesystem is
// fake_vfs.h.
//
// One thread plays every task. Where a task waits — for the filesystem lock, or
// in vTaskDelay — a hook lets the test play the other task. The slot-table
// mutex double then answers a take the way FreeRTOS would: a bounded take of a
// mutex whose holder is waiting further up the stack misses after its timeout;
// an unbounded one could never return and is counted as a deadlock.
//
// The radio is a loopback: a frame the sender hands to the smart send path is
// delivered to the receive handlers at once when the session is ACTIVE, and
// otherwise goes to a model of the pending-frame store. FILE_START and FILE_END
// go through the acknowledged send (v4_send_encrypted_chunked with ackSingle):
// the double hands the frame to the receive handler and turns its verdict into
// the outcome the sender sees (accepted: Delivered, refused: Refused(status),
// silent or a lost ACK: NoAnswer). The receiver's FILE_RESULT and FILE_CANCEL
// reach the sender's handlers the same way. The session handshake completes
// only when the test says the command task is free, never inside the pump,
// because on the device the handshake is finished by a job on the task the
// pump is running on.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <Arduino.h>            // the shared host String double, extended by the suite
#include "System_BuildConfig.h" // the suite's stub: ESP-NOW on, bond/OLED/automation off
void webBatchTestStringAppend() {}

static unsigned checks = 0;
static unsigned failures = 0;
static void check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

// ---- clock, logging ---------------------------------------------------------
static uint32_t nowMs = 100000;
uint32_t millis() { return nowMs; }
static void logStub(const char*, ...) {}
#define DEBUG_ESPNOWF(...)        logStub(__VA_ARGS__)
#define DEBUG_ESPNOW_ROUTERF(...) logStub(__VA_ARGS__)
#define DEBUG_ESPNOW_STREAMF(...) logStub(__VA_ARGS__)
#define INFO_ESPNOWF(...)         logStub(__VA_ARGS__)
#define WARN_ESPNOWF(...)         logStub(__VA_ARGS__)
#define ERROR_ESPNOWF(...)        logStub(__VA_ARGS__)
#define EXT_RAM_BSS_ATTR
static std::vector<std::string> broadcasts;
static void broadcastPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void broadcastPrintf(const char* fmt, ...) {
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  broadcasts.push_back(buf);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
void broadcastOutput(const String& s) { broadcasts.push_back(s.c_str()); }

// ---- FreeRTOS: the slot-table mutex and the pacing delay ---------------------
using TickType_t = uint32_t;
static constexpr TickType_t portMAX_DELAY = 0xffffffffu;
static constexpr int pdTRUE = 1;
static constexpr int pdFALSE = 0;
static constexpr TickType_t pdMS_TO_TICKS(TickType_t ms) { return ms; }
struct HostMutex {
  bool held = false;
};
using SemaphoreHandle_t = HostMutex*;
static HostMutex slotMutex;
// Another task keeps the slot lock for this long from now.
static uint32_t slotLockHeldElsewhereMs = 0;
// n > 0: the n-th take from now misses once if it is a bounded take.
static int slotLockMissAtTake = 0;
static unsigned boundedMisses = 0, deadlocks = 0;
SemaphoreHandle_t xSemaphoreCreateMutex() { return &slotMutex; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
  if (m->held) {
    // The holder is a task that is waiting further up this stack.
    if (wait == portMAX_DELAY) { ++deadlocks; return pdFALSE; }
    nowMs += wait; ++boundedMisses; return pdFALSE;
  }
  if (slotLockMissAtTake > 0 && --slotLockMissAtTake == 0 && wait != portMAX_DELAY) {
    nowMs += wait; ++boundedMisses; return pdFALSE;
  }
  if (slotLockHeldElsewhereMs) {
    if (wait != portMAX_DELAY && wait < slotLockHeldElsewhereMs) {
      slotLockHeldElsewhereMs -= wait; nowMs += wait; ++boundedMisses; return pdFALSE;
    }
    nowMs += slotLockHeldElsewhereMs;
    slotLockHeldElsewhereMs = 0;
  }
  m->held = true;
  return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t m) {
  if (!m->held) check(false, "slot mutex given back while not held");
  m->held = false;
}
static std::function<void()> onTaskDelay;  // the other tasks run while this one sleeps
void vTaskDelay(TickType_t ticks) {
  nowMs += ticks;
  if (onTaskDelay) onTaskDelay();
}

// ---- PSRAM helpers -------------------------------------------------------------
enum class AllocPref { PreferPsram, RequirePsram, DefaultHeap };
static long liveAllocations = 0;  // transfer buffers; the slot table itself is taken off in main
void* ps_alloc(size_t n, AllocPref, const char*) { ++liveAllocations; return std::malloc(n ? n : 1); }
void ps_free(void* p) { if (p) --liveAllocations; std::free(p); }

// ---- filesystem: the shared fake, plus the filesystem lock ----------------------
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#include "fake_vfs.h"
#include <ArduinoJson.h>
#include "psram_json_doc_host.h"

static int fsLockDepth = 0;
// FILE-3: VFS::admitBulkWrite's rule over a free figure the test sets. The
// production function is run by test_bulk_write_admission.py; this double
// keeps the arithmetic (free >= reserve + bytes, an unknown figure passes) and
// records how the receive path asks: with which figure, for how many bytes,
// and whether a lock was held.
static size_t fakeLittleFsFree = SIZE_MAX;          // SIZE_MAX: nothing sampled yet
static unsigned admitPeeks = 0, admitRefreshes = 0, admitRefused = 0;
static unsigned admitPeeksUnderFsLock = 0, admitRefreshesWithoutFsLock = 0, admitRefreshesUnderSlotLock = 0;
static std::vector<size_t> admitAsks;
static constexpr size_t kFakeReserve = 100 * 1024;
// What the receive path said it put on flash since the figure was "read"
// (VFS::noteBulkWriteTaken): the double takes it off the figure, as the
// production rule does, and counts the calls made with the slot lock free of
// filesystem work.
static size_t fakeTakenBytes = 0;
static std::vector<size_t> takenNotes;
namespace VFS {
enum class BulkFigure : uint8_t { Peek, Refresh, RefreshIfShort };
inline void noteBulkWriteTaken(size_t bytes) { fakeTakenBytes += bytes; takenNotes.push_back(bytes); }
inline bool admitBulkWrite(StorageType tier, size_t bytes, const char* why,
                           BulkFigure figure = BulkFigure::Peek, size_t* roomBytes = nullptr,
                           size_t wantBytes = 0) {
  (void)why; (void)wantBytes;
  if (roomBytes) *roomBytes = SIZE_MAX;
  if (tier == SDCARD) return true;
  if (figure == BulkFigure::Peek) {
    ++admitPeeks;
    if (fsLockDepth != 0) ++admitPeeksUnderFsLock;
  } else {
    ++admitRefreshes;
    if (fsLockDepth == 0) ++admitRefreshesWithoutFsLock;
    if (slotMutex.held) ++admitRefreshesUnderSlotLock;
  }
  admitAsks.push_back(bytes);
  if (fakeLittleFsFree == SIZE_MAX) return true;
  const size_t freeNow = fakeLittleFsFree > fakeTakenBytes ? fakeLittleFsFree - fakeTakenBytes : 0;
  const size_t room = freeNow > kFakeReserve ? freeNow - kFakeReserve : 0;
  if (roomBytes) *roomBytes = room;
  if (bytes <= room) return true;
  ++admitRefused;
  return false;
}
}  // namespace VFS
// Runs once, while the caller waits for the filesystem lock (a long directory
// walk on another task holds it): the test plays espnow_task in here.
static std::function<void()> onFsLockWait;
static unsigned fsLockWaitsUnderSlotLock = 0;  // waited for the FS lock holding the slot lock
static unsigned fsCallsUnderSlotLock = 0;      // filesystem work holding the slot lock
struct FsLockGuard {
  explicit FsLockGuard(const char* = nullptr) {
    if (slotMutex.held) ++fsLockWaitsUnderSlotLock;
    if (onFsLockWait && fsLockDepth == 0) {
      auto hook = std::move(onFsLockWait);
      onFsLockWait = nullptr;
      hook();
    }
    ++fsLockDepth;
  }
  ~FsLockGuard() { --fsLockDepth; }
  FsLockGuard(const FsLockGuard&) = delete;
  FsLockGuard& operator=(const FsLockGuard&) = delete;
};
static std::function<void(const fake_vfs::Call&)> onFsCall;
static void wireFs() {
  auto& fs = fake_vfs::fs();
  fs.lockHeld = [] { return fsLockDepth > 0; };
  fs.onCall = [](const fake_vfs::Call& call) {
    if (slotMutex.held) ++fsCallsUnderSlotLock;
    if (onFsCall) onFsCall(call);
  };
}

// esp_crc32_le as the ROM computes it: zlib CRC-32, seed 0 for a fresh run.
uint32_t esp_crc32_le(uint32_t crc, const uint8_t* buf, uint32_t len) {
  uint32_t c = ~crc;
  for (uint32_t i = 0; i < len; i++) {
    c ^= buf[i];
    for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// ---- the receiving device's command task ----------------------------------------
struct ExecReq {
  using DeferredFn = void (*)(void*);
};
struct CmdExecJob {
  ExecReq::DeferredFn fn;
  void* arg;
};
static std::deque<CmdExecJob> cmdExecQueue;
static int cmdExecRefuseNext = 0;  // the next n submits find the queue full
static std::function<void()> onCmdExecRefused;
bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg) {
  if (cmdExecRefuseNext > 0) {
    --cmdExecRefuseNext;
    if (onCmdExecRefused) onCmdExecRefused();
    return false;
  }
  cmdExecQueue.push_back({fn, arg});
  return true;
}
static void runCmdExec() {
  while (!cmdExecQueue.empty()) {
    CmdExecJob job = cmdExecQueue.front();
    cmdExecQueue.pop_front();
    job.fn(job.arg);
  }
}

// ---- wire ------------------------------------------------------------------------
#include "System_MacText.h"
#include "System_ESPNow_Wire.h"
#include "System_ESPNow_RxCtx.h"

// INSERT_RESULT_TYPES

// ---- production: receive slot table ------------------------------------------------
// INSERT_SLOTS_HEADER
// INSERT_SLOTS_SOURCE

// ---- device state and the radio ------------------------------------------------------
struct EspNowState {
  bool initialized = true;
  bool encryptionEnabled = true;
  volatile bool fileSendInProgress = false;
  uint8_t fileSendPeer[6] = {};
  uint32_t fileSendStartedMs = 0;
  volatile uint32_t fileSendTransferId = 0;
  volatile uint32_t fileSendAbortTransferId = 0;
  volatile uint8_t fileSendAbortStatus = 0;
  volatile uint32_t fileSendResultTransferId = 0;
  volatile uint8_t fileSendResultStatus = 0;
  volatile uint32_t fileSendResultBytes = 0;
  uint32_t fileTransfersSent = 0;
  uint32_t fileTransfersReceived = 0;
};
static EspNowState gEspNowStorage;
static EspNowState* gEspNow = &gEspNowStorage;

// The two devices. The sender's frames reach the receive handlers with the
// sender's address as source; the receiver's FILE_CANCEL and FILE_RESULT reach
// the sender's handlers with the receiver's address as source.
static uint8_t kSender[6]   = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0A};
static uint8_t kReceiver[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0B};
static uint8_t kOther[6]    = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0C};

enum LogMessageType {
  MSG_FILE_SEND_START, MSG_FILE_SEND_SUCCESS, MSG_FILE_SEND_FAILED,
  MSG_FILE_RECV_SUCCESS, MSG_FILE_RECV_FAILED,
};
struct TransferEvent {
  LogMessageType type;
  std::string name;
};
static std::vector<TransferEvent> events;
void logFileTransferEvent(uint8_t*, const char*, const char* filename, LogMessageType type) {
  events.push_back({type, filename ? filename : ""});
}
static unsigned eventCount(LogMessageType type) {
  unsigned n = 0;
  for (const auto& e : events) if (e.type == type) ++n;
  return n;
}
String macToDisplayStr(const uint8_t* mac) {
  char buf[18];
  macToDisplay(mac, buf, sizeof(buf));
  return String(buf);
}
String getEspNowDeviceName(const uint8_t* mac) {
  if (std::memcmp(mac, kReceiver, 6) == 0) return String("peerB");
  if (std::memcmp(mac, kSender, 6) == 0) return String("peerA");
  return String();
}

// A FILE_CANCEL from the receiver as production's v4_send_file_cancel puts it
// on the air (rule F3): `id` is the payload's reqId (the transferId), `reason`
// its status, `frameMsgId` the cancel frame's own header msgId.
struct Cancel {
  uint8_t to[6];
  uint32_t id;
  uint8_t reason;
  uint32_t frameMsgId;
};
static std::vector<Cancel> cancels;
// A FILE_RESULT from the receiver (v4_send_file_result).
struct Result {
  uint8_t to[6];
  uint32_t id;
  uint8_t status;
  uint32_t bytes;
  uint32_t frameMsgId;
  uint16_t flags;
};
static std::vector<Result> results;
static int dropResults = 0;          // the next n FILE_RESULTs are lost on air (-1: all)
static std::vector<uint32_t> acks;   // transferIds of the FILE_ENDs the receiver took
struct RxVerdict { uint8_t type; uint32_t msgId; V4Verdict v; };
static std::vector<RxVerdict> verdicts;
static bool loopback = false;  // receiver replies are delivered to the sender
static V4Verdict rxFrame(uint8_t type, uint8_t* src, uint32_t msgId, const uint8_t* payload, uint16_t len);
bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                           const uint8_t* payload, uint16_t len, uint8_t ttl);
uint32_t generateMessageId();
static unsigned borrowedCancelIds = 0;
// The receiver's replies (the smart-send double for a destination other than
// the receiver): FILE_CANCEL and FILE_RESULT, delivered to the sender's
// handlers when the loopback is on and the reply is addressed to it.
static bool receiverReply(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t frameMsgId,
                          const uint8_t* payload, uint16_t len) {
  const bool toSender = std::memcmp(dst, kSender, 6) == 0;
  if (type == ESPNOW_V4_TYPE_FILE_CANCEL) {
    Cancel c{};
    std::memcpy(c.to, dst, 6);
    c.frameMsgId = frameMsgId;
    check(len == sizeof(V4PayloadFileCancel) && len == espnowanswer::kHeaderLen &&
              espnowanswer::parseHeader(payload, len, &c.id, &c.reason),
          "FILE_CANCEL is the 5-byte answer {reqId, status}");
    if (frameMsgId == c.id) ++borrowedCancelIds;
    cancels.push_back(c);
    if (loopback && toSender) rxFrame(type, kReceiver, frameMsgId, payload, len);
    return true;
  }
  if (type == ESPNOW_V4_TYPE_FILE_RESULT) {
    Result r{};
    std::memcpy(r.to, dst, 6);
    r.frameMsgId = frameMsgId;
    r.flags = flags;
    V4PayloadFileResult p{};
    check(len == sizeof(p), "FILE_RESULT is 12 bytes");
    std::memcpy(&p, payload, sizeof(p));
    r.id = p.reqId;
    r.status = p.status;
    r.bytes = p.bytesStored;
    check(r.id != frameMsgId, "FILE_RESULT: its header msgId is the receiver's own, not the transferId");
    results.push_back(r);
    if (dropResults != 0) {
      if (dropResults > 0) --dropResults;
      return true;  // lost on the air
    }
    if (loopback && toSender) rxFrame(type, kReceiver, frameMsgId, payload, len);
    return true;
  }
  check(false, "receiver reply: unexpected frame type");
  return false;
}

// ---- production: the FILE_CANCEL and FILE_RESULT senders ----------------------------------
// INSERT_CANCEL_TX
// 1.0 step 5b: no handler sends an ACK of its own. A frame's ACK is the receive
// path's answer to its verdict: rxFrame records the transferId of an accepted
// FILE_END in `acks` (and every verdict in `verdicts`).
static unsigned cancelCount(uint32_t id) {
  unsigned n = 0;
  for (const auto& c : cancels) if (c.id == id) ++n;
  return n;
}
static unsigned cancelCount(uint32_t id, uint8_t reason) {
  unsigned n = 0;
  for (const auto& c : cancels) if (c.id == id && c.reason == reason) ++n;
  return n;
}
static unsigned resultCount(uint32_t id) {
  unsigned n = 0;
  for (const auto& r : results) if (r.id == id) ++n;
  return n;
}
static unsigned resultCount(uint32_t id, uint8_t status) {
  unsigned n = 0;
  for (const auto& r : results) if (r.id == id && r.status == status) ++n;
  return n;
}
static const Result* lastResult(uint32_t id) {
  for (size_t i = results.size(); i-- > 0;) if (results[i].id == id) return &results[i];
  return nullptr;
}

// ---- production: receive handlers ---------------------------------------------------------
// INSERT_RX

// One received frame, as the dispatcher hands it to a handler (after the row's
// minLen check, which the double applies too).
static bool gHoldLoopPass = false;
static uint16_t minLenOf(uint8_t type) {
  switch (type) {
    case ESPNOW_V4_TYPE_FILE_START:  return ESPNOW_V4_MINLEN_FILE_START;
    case ESPNOW_V4_TYPE_FILE_DATA:   return ESPNOW_V4_MINLEN_FILE_DATA;
    case ESPNOW_V4_TYPE_FILE_END:    return ESPNOW_V4_MINLEN_FILE_END;
    case ESPNOW_V4_TYPE_FILE_RESULT: return ESPNOW_V4_MINLEN_FILE_RESULT;
    case ESPNOW_V4_TYPE_FILE_CANCEL: return ESPNOW_V4_MINLEN_FILE_CANCEL;
    default: return 0;
  }
}
static V4Verdict rxFrame(uint8_t type, uint8_t* src, uint32_t msgId, const uint8_t* payload, uint16_t len) {
  esp_now_recv_info info{};
  info.src_addr = src;
  EspNowV4Header h{};
  h.type = type;
  h.msgId = msgId;
  V4RxCtx ctx{};
  ctx.recv_info = &info;
  ctx.h = &h;
  ctx.payload = payload;
  ctx.payloadLen = len;
  ctx.isPaired = ctx.isAuthenticated = ctx.isSessionEncrypted = true;
  ctx.deviceName = "peer";
  V4Verdict v = v4RefusedStatus(espnowanswer::kBadRequest);  // the dispatcher's length rule
  if (len >= minLenOf(type)) {
    switch (type) {
      case ESPNOW_V4_TYPE_FILE_START:  v = v4h_file_start(ctx); break;
      case ESPNOW_V4_TYPE_FILE_DATA:   v = v4h_file_data(ctx); break;
      case ESPNOW_V4_TYPE_FILE_END:    v = v4h_file_end(ctx); break;
      case ESPNOW_V4_TYPE_FILE_RESULT: v = v4h_file_result(ctx); break;
      case ESPNOW_V4_TYPE_FILE_CANCEL: v = v4h_file_cancel(ctx); break;
      default: check(false, "rxFrame: unexpected frame type");
    }
  }
  verdicts.push_back({type, msgId, v});
  if (type == ESPNOW_V4_TYPE_FILE_END && v.kind == V4VerdictKind::Accepted) {
    uint32_t id = 0;
    std::memcpy(&id, payload, sizeof(id));
    acks.push_back(id);
  }
  // The espnow_task loop's next pass finishes a small file after its ACK
  // (a case can hold that pass to look between the two).
  if (type == ESPNOW_V4_TYPE_FILE_END && !gHoldLoopPass) fileRamFinalizeTick();
  return v;
}

// ---- sessions, the pending-frame store and the smart send path ----------------------------
enum SessionStateKind : uint8_t {
  SESSION_FREE = 0, SESSION_ESTABLISHING = 1, SESSION_ACTIVE = 2, SESSION_REKEYING = 3, SESSION_CLOSED = 4,
};
struct PeerIdentity {
  uint8_t mac[6];
  uint8_t meshId;
};
struct SessionState {
  uint8_t state;
};
static PeerIdentity receiverIdentity{{0x02, 0x00, 0x00, 0x00, 0x00, 0x0B}, 0};
static bool receiverHasIdentity = true;
static bool receiverHasSessionSlot = true;  // false: sessionFindByPeer finds nothing
static SessionState receiverSession{SESSION_ACTIVE};
static unsigned sessionOpenKicks = 0, keyExKicks = 0;
static bool keyExInFlight = false;
const PeerIdentity* peerIdentityFindByMac(const uint8_t* mac) {
  return (receiverHasIdentity && std::memcmp(mac, kReceiver, 6) == 0) ? &receiverIdentity : nullptr;
}
SessionState* sessionFindByPeer(const uint8_t* mac, uint8_t) {
  return (receiverHasSessionSlot && std::memcmp(mac, kReceiver, 6) == 0) ? &receiverSession : nullptr;
}
// As the device does it: sessionAllocate resets the slot to ESTABLISHING, whatever it held.
static bool kicksFail = false;   // no shared mesh key, no free slot, radio failure
bool espnowSessionOpenInitiate(const uint8_t*, const char*) {
  ++sessionOpenKicks;
  if (kicksFail) return false;
  receiverHasSessionSlot = true;
  receiverSession.state = SESSION_ESTABLISHING;
  return true;
}
bool keyExIsInFlight(const uint8_t*) { return keyExInFlight; }
bool espnowKeyExInitiate(const uint8_t*, const char*) {
  ++keyExKicks;
  if (kicksFail) return false;
  keyExInFlight = true;
  return true;
}

struct AirFrame {
  uint8_t type;
  uint32_t msgId;
  std::vector<uint8_t> payload;
  uint16_t flags;
};
static std::vector<AirFrame> air;        // frames that left the sender
// The pending-frame store (System_ESPNow_PendingQueue.h): up to 4 frames for
// the one receiver, sent oldest first when the session opens; a fifth is
// refused.
constexpr uint8_t kPendingFramesPerPeer = 4;
constexpr uint8_t kPendingFrameSlots = 16;
static std::vector<AirFrame> pendingFrames;
static unsigned pendingParked = 0;       // frames ever parked
static bool pendingInUse() { return !pendingFrames.empty(); }
bool pendingFrameIsWaiting(uint32_t msgId, const uint8_t*) {
  for (const AirFrame& f : pendingFrames) {
    if (f.msgId == msgId) return true;
  }
  return false;
}
uint8_t pendingFrameCountForPeer(const uint8_t*) { return static_cast<uint8_t>(pendingFrames.size()); }
uint8_t pendingFrameCount() { return static_cast<uint8_t>(pendingFrames.size()); }
static std::function<void(const AirFrame&)> onAirFrame;
// Changes a frame between the sender and the receiver (a corrupted chunk).
static std::function<void(AirFrame&)> mutateOnAir;
static V4Verdict putOnAir(const AirFrame& sent) {
  air.push_back(sent);
  if (onAirFrame) onAirFrame(sent);
  if (!loopback) return v4Silent();
  AirFrame f = sent;
  if (mutateOnAir) mutateOnAir(f);
  return rxFrame(f.type, kSender, f.msgId, f.payload.data(), static_cast<uint16_t>(f.payload.size()));
}
// v4_send_payload_smart for one frame to the receiver: session-wrapped at once
// when the session is ACTIVE, else parked behind the frames already waiting
// (refused when 4 wait) with a handshake kick when none is in flight.
bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                           const uint8_t* payload, uint16_t len, uint8_t) {
  if (std::memcmp(dst, kReceiver, 6) != 0) return receiverReply(dst, type, flags, msgId, payload, len);
  AirFrame f{type, msgId, std::vector<uint8_t>(payload, payload + len), flags};
  SessionState* s = sessionFindByPeer(dst, 0);
  if (peerIdentityFindByMac(dst) && s && s->state == SESSION_ACTIVE) {
    putOnAir(f);
    return true;
  }
  if (pendingFrames.size() >= kPendingFramesPerPeer) return false;
  pendingFrames.push_back(f);
  ++pendingParked;
  if (!peerIdentityFindByMac(dst)) {
    if (!keyExIsInFlight(dst)) espnowKeyExInitiate(dst, nullptr);
  } else if (!s || s->state == SESSION_FREE || s->state == SESSION_CLOSED) {
    espnowSessionOpenInitiate(dst, nullptr);
  }
  return true;
}
// The command task is free again: the handshake job runs, the session becomes
// ACTIVE and the parked frames are sent, oldest first.
static void commandTaskCompletesHandshake() {
  receiverHasIdentity = true;
  receiverHasSessionSlot = true;
  keyExInFlight = false;
  receiverSession.state = SESSION_ACTIVE;
  std::vector<AirFrame> waiting;
  waiting.swap(pendingFrames);
  for (const AirFrame& f : waiting) putOnAir(f);
}
static bool routedPeer = false;  // the receiver is reached through a relay
uint16_t v4_single_frame_cap(const uint8_t*) {
  return routedPeer ? ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT : ESPNOW_V4_MAX_PLAINTEXT;
}

// The acknowledged send (v4_send_encrypted_chunked with ackSingle) of
// FILE_START and FILE_END: up to three tries 200 ms apart on the device; here
// the receive handler's verdict is the ACK. `lostAcks` drops the next n ACKs
// (the receiver took the frame, the sender never heard it).
enum class V4SendOutcome : uint8_t { Delivered = 0, Refused = 1, NoAnswer = 2, NotSent = 3 };
struct V4SendResult {
  V4SendOutcome outcome;
  uint8_t status;
};
static int lostAcks = 0;
static unsigned ackedSends = 0;
V4SendResult v4_send_encrypted_chunked(const uint8_t dst[6], uint8_t type, uint16_t flags, uint32_t msgId,
                                       const uint8_t* payload, uint16_t len, uint8_t, bool ackSingle) {
  ++ackedSends;
  check(ackSingle && (flags & ESPNOW_V4_FLAG_ACK_REQ) != 0, "FILE_START/END wait for their ACK");
  check(std::memcmp(dst, kReceiver, 6) == 0, "acknowledged send: unexpected destination");
  SessionState* s = sessionFindByPeer(dst, 0);
  if (!peerIdentityFindByMac(dst) || !s || s->state != SESSION_ACTIVE) return {V4SendOutcome::NotSent, 0};
  const V4Verdict v = putOnAir(AirFrame{type, msgId, std::vector<uint8_t>(payload, payload + len), flags});
  if (!loopback) return {V4SendOutcome::NoAnswer, 0};
  if (lostAcks > 0) { --lostAcks; nowMs += 600; return {V4SendOutcome::NoAnswer, 0}; }
  if (v.kind == V4VerdictKind::Accepted) return {V4SendOutcome::Delivered, 0};
  if (v.kind == V4VerdictKind::Refused) return {V4SendOutcome::Refused, v.status};
  nowMs += 600;  // three tries, no answer
  return {V4SendOutcome::NoAnswer, 0};
}
uint32_t esp_random() {
  static uint32_t x = 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return x;
}

static AuthContext callerAuth;
const AuthContext& currentAuthContext() { return callerAuth; }
static bool readAllowed = true;
bool canRead(const String&, const AuthContext&) { return readAllowed; }
static uint32_t nextMessageId = 500;
uint32_t generateMessageId() { return ++nextMessageId; }

// ---- production: sender pump -----------------------------------------------------------------
// INSERT_TX


// ---- command-layer doubles ----------------------------------------------------------------------
bool gCliValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (::gCliValidateOnly) return "VALID"; } while (0)
#define SYSTEM_IDENTITY_SCOPE(purpose) ((void)0)
#define FILE_MANAGER_MAX_PATH 128
#define ESPNOW_TEXT_MAX_LEN 1024
using esp_err_t = int;
esp_err_t espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, kSender, 6); return 0; }
esp_err_t espnowSelfApMac(uint8_t out[6]) { std::memcpy(out, kSender, 6); out[5] ^= 0x40; return 0; }
bool resolveDeviceNameOrMac(const String& name, uint8_t* out) {
  if (name != "peerB") return false;
  std::memcpy(out, kReceiver, 6);
  return true;
}
bool isMeshMode() { return false; }
bool espnowPeerExists(const uint8_t*) { return true; }
static std::vector<std::string> hints, toasts;
void cliHint(const char* text) { hints.push_back(text); }
namespace oled_message { constexpr uint32_t kErrorDurationMs = 8000; }
enum class OledUiIcon : uint8_t { NONE, INFO, WARNING, ERROR, SUCCESS, QUESTION };
void oledToastShow(const char* text, uint32_t = 3000, OledUiIcon = OledUiIcon::NONE) { toasts.push_back(text); }
static char debugBuffer[1024];
bool ensureDebugBuffer() { return true; }
char* getDebugBuffer() { return debugBuffer; }
static unsigned statusRegistered = 0, statusFailed = 0, historyStored = 0;
static uint32_t lastRegisteredMsgId = 0;
static unsigned historyStoredAtStamp = 0;  // historyStored when the last delivery stamp came
void sendStatusRegister(uint32_t msgId, const uint8_t*) { ++statusRegistered; lastRegisteredMsgId = msgId; }
void sendStatusMarkFailed(uint32_t, const uint8_t*) { ++statusFailed; }
bool storeSentMessageInPeerHistory(uint8_t*, const char*, uint32_t) { ++historyStored; return true; }
// After the sent record is written, espnowsend re-reads the send status and
// stamps a state that resolved before the record existed.
enum SendStatusState : uint8_t { SEND_STATUS_PENDING = 0, SEND_STATUS_DELIVERED = 1 };
struct SendStatus { bool inUse; uint8_t state; uint8_t peerMac[6]; uint32_t msgId; uint32_t registeredAtMs;
                    uint32_t resolvedAtMs; uint8_t status; };
static unsigned statusReads = 0, deliveryStamps = 0;
static uint8_t statusState = SEND_STATUS_PENDING;  // what the status table says when re-read
static uint32_t stampMsgId = 0;
static uint8_t stampState = 0xFF, stampMac[6] = {};
bool sendStatusGet(uint32_t msgId, SendStatus* out) {
  ++statusReads;
  if (out) *out = SendStatus{true, statusState, {}, msgId, 0, 0, 0};
  return true;
}
void espnowUpdateSentDeliveryState(const uint8_t* mac, uint32_t msgId, uint8_t state, uint8_t = 0) {
  ++deliveryStamps;
  historyStoredAtStamp = historyStored;
  stampMsgId = msgId;
  stampState = state;
  if (mac) std::memcpy(stampMac, mac, 6);
}
// The tx dispatcher's synchronous send of a TEXT message: one frame goes the
// smart single-frame way; a longer one needs an ACTIVE session, and without it
// the smart path starts a handshake (restarting one that is in flight) and fails.
static unsigned aeadSyncCalls = 0;
namespace espnowtx {
bool sendAeadSync(const uint8_t* mac, uint8_t type, uint16_t flags, uint32_t msgId,
                  const uint8_t* payload, uint16_t len, uint8_t ttl, uint32_t) {
  ++aeadSyncCalls;
  if (len <= v4_single_frame_cap(mac)) return v4_send_payload_smart(mac, type, flags, msgId, payload, len, ttl);
  const PeerIdentity* pid = peerIdentityFindByMac(mac);
  SessionState* s = pid ? sessionFindByPeer(mac, pid->meshId) : nullptr;
  if (s && s->state == SESSION_ACTIVE) {
    air.push_back({type, msgId, std::vector<uint8_t>(payload, payload + len), flags});
    return true;
  }
  if (!pid) espnowKeyExInitiate(mac, nullptr);
  else espnowSessionOpenInitiate(mac, nullptr);
  return false;
}
}  // namespace espnowtx

// ---- production: CommandArgs and the commands that report a send ----------------------------------
// INSERT_COMMANDS

// =====================================================================================================
// Helpers
// =====================================================================================================
static const char* const kNoSessionPeerB =
    "Error: no secure session with peerB yet; one is being opened, try again in a few seconds";

static std::string pattern(size_t n, uint8_t seed) {
  std::string out(n, '\0');
  uint32_t x = 0x1234567u + seed;
  for (size_t i = 0; i < n; i++) {
    x = x * 1664525u + 1013904223u;
    out[i] = static_cast<char>(x >> 24);
  }
  return out;
}
static uint32_t crcOf(const std::string& bytes) {
  return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size()));
}
static std::string inboxPath(const uint8_t* from, const char* name) {
  char token[13];
  macToPathToken(from, token);
  return std::string("/espnow/received/") + token + "/" + name;
}
static constexpr uint16_t kChunk = ESPNOW_V4_FILE_CHUNK_DIRECT;  // what the sender uses to a direct peer
static uint16_t chunksOf(size_t size) { return static_cast<uint16_t>((size + kChunk - 1) / kChunk); }

// Each frame a test plays has a header msgId of its own (1.0 step 7); the
// transfer is named by the transferId in the payload.
static uint32_t nextFrameMsgId = 0x700000;
static V4Verdict rxStartNamed(uint8_t* src, uint32_t id, const std::string& name, size_t size,
                              uint8_t kind = V4_FILE_KIND_FILE, uint16_t chunk = kChunk) {
  std::vector<uint8_t> p(sizeof(V4PayloadFileStart) + name.size());
  V4PayloadFileStart s{};
  s.transferId = id;
  s.totalSize = static_cast<uint32_t>(size);
  s.chunkSize = chunk;
  s.kind = kind;
  s.nameLen = static_cast<uint8_t>(name.size());
  std::memcpy(p.data(), &s, sizeof(s));
  std::memcpy(p.data() + sizeof(s), name.data(), name.size());
  return rxFrame(ESPNOW_V4_TYPE_FILE_START, src, ++nextFrameMsgId, p.data(), static_cast<uint16_t>(p.size()));
}
static V4Verdict rxStart(uint8_t* src, uint32_t id, const char* name, size_t size) {
  return rxStartNamed(src, id, name, size);
}
static void rxChunk(uint8_t* src, uint32_t id, const std::string& bytes, uint16_t index) {
  V4PayloadFileData d{};
  d.transferId = id;
  d.chunkIndex = index;
  const size_t offset = static_cast<size_t>(index) * kChunk;
  const size_t n = std::min<size_t>(kChunk, bytes.size() - offset);
  std::memcpy(d.data, bytes.data() + offset, n);
  rxFrame(ESPNOW_V4_TYPE_FILE_DATA, src, ++nextFrameMsgId, reinterpret_cast<const uint8_t*>(&d),
          static_cast<uint16_t>(offsetof(V4PayloadFileData, data) + n));
}
// A chunk of any length (a peer that does not keep to FILE_START's chunkSize).
static V4Verdict rxChunkRaw(uint8_t* src, uint32_t id, uint32_t index, size_t len) {
  V4PayloadFileData d{};
  d.transferId = id;
  d.chunkIndex = index;
  std::memset(d.data, 0xA5, sizeof(d.data));
  return rxFrame(ESPNOW_V4_TYPE_FILE_DATA, src, ++nextFrameMsgId, reinterpret_cast<const uint8_t*>(&d),
                 static_cast<uint16_t>(offsetof(V4PayloadFileData, data) + len));
}
// FILE_END's status: 0 = the sender sent the whole file (0 is ok in every
// status byte, rule F3); anything else (kCancelled) = it stopped.
static V4Verdict rxEnd(uint8_t* src, uint32_t id, uint32_t crc, uint32_t count, uint8_t status = espnowanswer::kOk) {
  V4PayloadFileEnd e{};
  e.transferId = id;
  e.chunkCount = count;
  e.crc32 = crc;
  e.status = status;
  return rxFrame(ESPNOW_V4_TYPE_FILE_END, src, ++nextFrameMsgId, reinterpret_cast<const uint8_t*>(&e), sizeof(e));
}
static V4Verdict rxEndOf(uint8_t* src, uint32_t id, const std::string& bytes) {
  return rxEnd(src, id, crcOf(bytes), chunksOf(bytes.size()));
}
static V4Verdict rxStop(uint8_t* src, uint32_t id) { return rxEnd(src, id, 0, 0, espnowanswer::kCancelled); }
// Chunks [from, to) of a streaming receive, the writer keeping up after each.
static void rxChunksWithWriter(uint8_t* src, uint32_t msgId, const std::string& bytes, uint16_t from, uint16_t to) {
  for (uint16_t i = from; i < to; i++) {
    rxChunk(src, msgId, bytes, i);
    runCmdExec();
  }
}
static unsigned partFiles() {
  unsigned n = 0;
  for (const auto& name : fake_vfs::fs().list("/espnow/received")) if (name.rfind(".part-", 0) == 0) ++n;
  return n;
}
static uint8_t slotState(uint32_t id) {
  for (uint8_t i = 0; i < fileSlotsSlotCount(); i++) {
    FileTransferSlotInfo info{};
    if (fileSlotsSnapshot(i, &info) && info.transferId == id) return info.state;
  }
  return FILE_SLOT_FREE;
}
static bool isRefused(const V4Verdict& v, uint8_t status) {
  return v.kind == V4VerdictKind::Refused && v.status == status;
}
static bool isAccepted(const V4Verdict& v) { return v.kind == V4VerdictKind::Accepted; }

static void resetWorld() {
  // Every slot must be free and every buffer returned between cases.
  check(fileSlotsActiveCount() == 0, "no slot in use between cases");
  check(liveAllocations == 0, "no transfer buffer left allocated between cases");
  check(!slotMutex.held, "slot mutex free between cases");
  check(fsLockDepth == 0, "filesystem lock free between cases");
  check(cmdExecQueue.empty(), "command-task queue empty between cases");
  fake_vfs::fs().reset();
  wireFs();
  fake_vfs::fs().mkdirAll("/espnow/received");
  onFsLockWait = nullptr;
  onFsCall = nullptr;
  onTaskDelay = nullptr;
  onAirFrame = nullptr;
  onCmdExecRefused = nullptr;
  slotLockHeldElsewhereMs = 0;
  slotLockMissAtTake = 0;
  boundedMisses = deadlocks = 0;
  fsLockWaitsUnderSlotLock = fsCallsUnderSlotLock = 0;
  cmdExecRefuseNext = 0;
  fakeLittleFsFree = SIZE_MAX;
  fakeTakenBytes = 0;
  takenNotes.clear();
  admitPeeks = admitRefreshes = admitRefused = 0;
  admitPeeksUnderFsLock = admitRefreshesWithoutFsLock = admitRefreshesUnderSlotLock = 0;
  admitAsks.clear();
  cancels.clear(); results.clear(); acks.clear(); verdicts.clear(); events.clear(); broadcasts.clear();
  borrowedCancelIds = 0;
  dropResults = 0; lostAcks = 0; ackedSends = 0; routedPeer = false;
  mutateOnAir = nullptr;
  fileSlotsForgetResults();
  air.clear(); pendingFrames.clear(); pendingParked = 0;
  loopback = false;
  gEspNowStorage = EspNowState{};
  receiverHasIdentity = true; receiverHasSessionSlot = true; keyExInFlight = false;
  receiverSession.state = SESSION_ACTIVE;
  sessionOpenKicks = keyExKicks = 0;
  kicksFail = false;
  readAllowed = true;
  hints.clear(); toasts.clear();
  statusRegistered = statusFailed = historyStored = aeadSyncCalls = 0;
  statusReads = deliveryStamps = historyStoredAtStamp = 0;
  statusState = SEND_STATUS_PENDING;
  lastRegisteredMsgId = stampMsgId = 0;
  stampState = 0xFF;
  std::memset(stampMac, 0, sizeof(stampMac));
  nowMs += 60000;  // clear of every rate limit of the case before
}

// =====================================================================================================
// Receive side
// =====================================================================================================
static const size_t kStreamSize = kFileSlotMaxFileSize + 1000;  // just over the RAM-path limit

static void testRamReceiveArrives() {
  resetWorld();
  const std::string bytes = pattern(1000, 1);
  check(isAccepted(rxStart(kSender, 11, "note.txt", bytes.size())), "RAM path: FILE_START is accepted");
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 11, bytes, i);
  rxEndOf(kSender, 11, bytes);
  check(fake_vfs::fs().text(inboxPath(kSender, "note.txt")) == bytes, "RAM path: the file arrives byte for byte");
  check(acks.size() == 1 && acks[0] == 11, "RAM path: FILE_END is acknowledged once");
  check(cancels.empty(), "RAM path: a healthy transfer gets no FILE_CANCEL");
  const Result* r = lastResult(11);
  check(results.size() == 1 && r && r->status == espnowanswer::kOk && r->bytes == bytes.size() &&
            std::memcmp(r->to, kSender, 6) == 0 && (r->flags & ESPNOW_V4_FLAG_ACK_REQ) != 0,
        "RAM path: one FILE_RESULT {stored, 1000 bytes} to the sender, with ACK_REQ");
  check(eventCount(MSG_FILE_RECV_SUCCESS) == 1 && eventCount(MSG_FILE_RECV_FAILED) == 0,
        "RAM path: one success record");
  check(fileSlotsActiveCount() == 0, "RAM path: the slot is released at FILE_END");
  check(partFiles() == 0, "RAM path: no staging file is left");
  check(fsCallsUnderSlotLock == 0 && fsLockWaitsUnderSlotLock == 0,
        "RAM path: no filesystem work with the slot lock held");
}

static void testStreamingReceiveArrives() {
  resetWorld();
  const std::string bytes = pattern(kStreamSize, 2);
  const uint16_t n = chunksOf(bytes.size());
  rxStart(kSender, 12, "big.bin", bytes.size());
  rxChunksWithWriter(kSender, 12, bytes, 0, n);
  rxEndOf(kSender, 12, bytes);
  check(slotState(12) == FILE_SLOT_COMPLETING, "streaming: FILE_END hands the slot to the finalize job");
  check(results.empty(), "streaming: no FILE_RESULT before the file is renamed into place");
  runCmdExec();
  check(fake_vfs::fs().text(inboxPath(kSender, "big.bin")) == bytes, "streaming: the file arrives byte for byte");
  check(acks.size() == 1 && acks[0] == 12, "streaming: the accepted FILE_END is acknowledged once");
  check(resultCount(12, espnowanswer::kOk) == 1 && lastResult(12)->bytes == bytes.size(),
        "streaming: the finalize job sends FILE_RESULT stored with the byte count");
  check(cancels.empty(), "streaming: a healthy transfer gets no FILE_CANCEL");
  check(fileSlotsActiveCount() == 0, "streaming: the finalize job frees the slot");
  check(partFiles() == 0, "streaming: no staging file is left");
  check(fake_vfs::fs().liveHandles() == 0, "streaming: the staging handle is closed");
  check(fsCallsUnderSlotLock == 0,
        "release does no filesystem work (close, remove) with the slot lock held");
  check(fsLockWaitsUnderSlotLock == 0, "release does not wait for the filesystem lock with the slot lock held");
  check(deadlocks == 0 && boundedMisses == 0, "streaming: no lock wait went wrong");
}

// F7 and the FILE_START checks: every refusal is the ACK's status, no slot is
// taken, no FILE_CANCEL is sent.
static void testStartRefusals() {
  resetWorld();
  check(isRefused(rxStartNamed(kSender, 1, std::string(97, 'a'), 100), espnowanswer::kTooLarge),
        "name over 96 bytes: kTooLarge (never cut)");
  check(isRefused(rxStartNamed(kSender, 2, std::string(64, 'b'), 100), espnowanswer::kTooLarge),
        "a name the LittleFS inbox cannot hold (64 > 63): kTooLarge at FILE_START");
  check(isAccepted(rxStartNamed(kSender, 3, std::string(63, 'c'), 100)), "a 63-byte name is accepted");
  rxStop(kSender, 3);
  check(isRefused(rxStartNamed(kSender, 4, "a/b.txt", 100), espnowanswer::kBadRequest), "a '/' in the name: kBadRequest");
  check(isRefused(rxStartNamed(kSender, 5, "..", 100), espnowanswer::kBadRequest), "'..' as the name: kBadRequest");
  check(isRefused(rxStartNamed(kSender, 6, std::string("a\0b", 3), 100), espnowanswer::kBadRequest),
        "a NUL inside the name: kBadRequest");
  check(isRefused(rxStartNamed(kSender, 61, "a\r\nb.txt", 100), espnowanswer::kBadRequest),
        "CR/LF in the name: kBadRequest (no forged log lines)");
  check(isRefused(rxStartNamed(kSender, 62, "\x1b[2Jx.txt", 100), espnowanswer::kBadRequest),
        "an escape sequence in the name: kBadRequest");
  check(isRefused(rxStartNamed(kSender, 63, "del\x7f.txt", 100), espnowanswer::kBadRequest), "DEL in the name: kBadRequest");
  check(isRefused(rxStartNamed(kSender, 7, "x.bin", 100, 2), espnowanswer::kUnsupported), "kind 2: kUnsupported");
  check(isRefused(rxStartNamed(kSender, 8, "_settings_out.json", 100, V4_FILE_KIND_BOND), espnowanswer::kUnsupported),
        "a bond answer in a build without bond mode: kUnsupported");
  check(isRefused(rxStartNamed(kSender, 0, "zero.bin", 100), espnowanswer::kBadRequest), "transferId 0: kBadRequest");
  check(isRefused(rxStartNamed(kSender, 9, "chunk.bin", 1000, V4_FILE_KIND_FILE, ESPNOW_V4_FILE_CHUNK_DIRECT + 1),
                  espnowanswer::kBadRequest),
        "a chunk larger than one FILE_DATA can carry: kBadRequest");
  check(isRefused(rxStartNamed(kSender, 10, "tiny.bin", 100000, V4_FILE_KIND_FILE, 1), espnowanswer::kBadRequest),
        "over 65,535 chunks: kBadRequest");
  check(isRefused(rxStart(kSender, 11, "huge.bin", kFileSlotMaxStreamSize + 1), espnowanswer::kTooLarge),
        "over the transfer limit: kTooLarge");
  {  // nameLen says more bytes than the frame has
    std::vector<uint8_t> p(sizeof(V4PayloadFileStart) + 3);
    V4PayloadFileStart s{};
    s.transferId = 12; s.totalSize = 10; s.chunkSize = kChunk; s.nameLen = 9;
    std::memcpy(p.data(), &s, sizeof(s));
    std::memcpy(p.data() + sizeof(s), "abc", 3);
    check(isRefused(rxFrame(ESPNOW_V4_TYPE_FILE_START, kSender, ++nextFrameMsgId, p.data(),
                            static_cast<uint16_t>(p.size())), espnowanswer::kBadRequest),
          "a name cut short by the frame: kBadRequest");
    check(isRefused(rxFrame(ESPNOW_V4_TYPE_FILE_START, kSender, ++nextFrameMsgId, p.data(), sizeof(s)),
                    espnowanswer::kBadRequest),
          "FILE_START without a name byte (below its 13-byte minimum): kBadRequest");
  }
  check(fileSlotsActiveCount() == 0 && liveAllocations == 0 && cancels.empty() && results.empty(),
        "refused starts: no slot, no buffer, no FILE_CANCEL, no FILE_RESULT");
}

// The 5b carry-over: a FILE_END copy after the slot was released gets the
// remembered FILE_RESULT, not kRefused.
static void testEndCopyGetsTheStoredResult() {
  resetWorld();
  const std::string bytes = pattern(700, 30);
  rxStart(kSender, 31, "copy.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 31, bytes, i);
  rxEndOf(kSender, 31, bytes);
  check(resultCount(31, espnowanswer::kOk) == 1 && fileSlotsActiveCount() == 0, "copy: stored, slot released");
  const V4Verdict again = rxEndOf(kSender, 31, bytes);
  check(isAccepted(again) && resultCount(31, espnowanswer::kOk) == 2 && lastResult(31)->bytes == bytes.size(),
        "a FILE_END copy after the release is taken and answered with the same FILE_RESULT");
  check(eventCount(MSG_FILE_RECV_SUCCESS) == 1 && fileSlotsActiveCount() == 0,
        "the copy stores nothing twice and opens no slot");
  // Another device asking for the same transferId gets nothing.
  check(isRefused(rxEndOf(kOther, 31, bytes), espnowanswer::kRefused) && resultCount(31) == 2,
        "the remembered result is only for the device that sent the transfer");
  // A failed verdict is remembered as well.
  rxStart(kSender, 32, "bad.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 32, bytes, i);
  rxEnd(kSender, 32, crcOf(bytes) ^ 1u, chunksOf(bytes.size()));
  rxEnd(kSender, 32, crcOf(bytes) ^ 1u, chunksOf(bytes.size()));
  check(resultCount(32, espnowanswer::kFileCrcMismatch) == 2, "a failed verdict (crc_mismatch) is answered again too");
  // A transfer never seen: refused; the sender's own stop for one: taken.
  check(isRefused(rxEndOf(kSender, 33, bytes), espnowanswer::kRefused), "FILE_END for an unknown transfer: kRefused");
  check(isAccepted(rxStop(kSender, 34)), "a stop for an unknown transfer: taken, nothing to do");
  // After the memory's minute the copy is a stranger.
  nowMs += kFileResultMemoryMs + 1;
  check(isRefused(rxEndOf(kSender, 31, bytes), espnowanswer::kRefused), "after 60 s the result is forgotten");
}

// FILE-3: a received file has to fit on LittleFS above the 100 KB reserve.
// The acceptance runs on espnow_task with the stored free figure (no walk, no
// filesystem lock) and answers a refusal on the FILE_START's ACK (io_error).
static void testStorageAdmissionAtAccept() {
  const size_t room = 5000;
  // N + 1 bytes with N free above the reserve: refused, storage reason.
  resetWorld();
  fakeLittleFsFree = kFakeReserve + room;
  const char* reason = "unset";
  check(fileSlotsAllocate(kSender, 50, "over.bin", room + 1, chunksOf(room + 1), kChunk, 0, &reason) == nullptr,
        "admission: a file one byte over the room is not given a slot");
  check(reason && std::strcmp(reason, kFileSlotErrStorage) == 0 && std::strcmp(reason, "NO_SPACE") == 0,
        "admission: the slot allocation reports the storage reason");
  check(admitPeeks == 1 && admitRefreshes == 0 && admitAsks.back() == room + 1,
        "admission: asked once for the file's size, with the stored figure");
  check(isRefused(rxStart(kSender, 51, "over.bin", room + 1), espnowanswer::kIoError) && cancels.empty() &&
            fileSlotsActiveCount() == 0,
        "admission: the FILE_START's ACK says io_error, no slot is held, no FILE_CANCEL");
  check(admitPeeksUnderFsLock == 0 && admitRefreshes == 0 && fsCallsUnderSlotLock == 0 && fsLockWaitsUnderSlotLock == 0,
        "admission: espnow_task takes no filesystem lock and walks nothing");
  check(liveAllocations == 0, "admission: a refused file allocates no buffer");
  // Exactly N bytes: accepted, and it arrives.
  const std::string fits = pattern(room, 7);
  rxStart(kSender, 52, "fits.bin", fits.size());
  for (uint16_t i = 0; i < chunksOf(fits.size()); i++) rxChunk(kSender, 52, fits, i);
  rxEndOf(kSender, 52, fits);
  check(cancelCount(52) == 0 && fake_vfs::fs().text(inboxPath(kSender, "fits.bin")) == fits,
        "admission: a file that exactly fits is received");
  // Below the reserve nothing is accepted, not even an empty file's worth.
  resetWorld();
  fakeLittleFsFree = kFakeReserve - 1;
  check(isRefused(rxStart(kSender, 53, "tiny.txt", 10), espnowanswer::kIoError) && fileSlotsActiveCount() == 0,
        "admission: below the reserve every file is refused");
  // No figure yet (first seconds of a boot): nothing to refuse on.
  resetWorld();
  const std::string early = pattern(700, 8);
  rxStart(kSender, 54, "early.txt", early.size());
  for (uint16_t i = 0; i < chunksOf(early.size()); i++) rxChunk(kSender, 54, early, i);
  rxEndOf(kSender, 54, early);
  check(admitPeeks == 1 && admitRefused == 0 && fake_vfs::fs().text(inboxPath(kSender, "early.txt")) == early,
        "admission: an unknown free figure admits");

  // Transfers in flight count: B fits alone but not beside A's unwritten bytes.
  resetWorld();
  fakeLittleFsFree = kFakeReserve + 3000;
  const std::string a = pattern(2000, 9), b = pattern(1500, 10);
  rxStart(kSender, 55, "a.bin", a.size());
  rxChunk(kSender, 55, a, 0);
  check(isRefused(rxStart(kOther, 56, "b.bin", b.size()), espnowanswer::kIoError) &&
            admitAsks.back() == a.size() + b.size(),
        "admission: the bytes other transfers still have to write are added");
  check(cancelCount(55) == 0 && slotState(55) == FILE_SLOT_RECEIVING, "admission: the transfer in flight is left alone");
  for (uint16_t i = 1; i < chunksOf(a.size()); i++) rxChunk(kSender, 55, a, i);
  rxEndOf(kSender, 55, a);
  check(fake_vfs::fs().text(inboxPath(kSender, "a.bin")) == a, "admission: and completes");

  // A retry that replaces its own stale transfer is not charged for it; one
  // that does not fit is refused without killing the transfer it would replace.
  resetWorld();
  fakeLittleFsFree = kFakeReserve + 2500;
  rxStart(kSender, 57, "retry.bin", a.size());
  rxChunk(kSender, 57, a, 0);
  check(isAccepted(rxStart(kSender, 58, "retry.bin", a.size())) && admitAsks.back() == a.size() &&
            slotState(58) == FILE_SLOT_RECEIVING,
        "admission: a retry (new transferId) is not charged for the transfer it replaces");
  check(isRefused(rxStart(kSender, 59, "retry.bin", 2600), espnowanswer::kIoError) &&
            slotState(58) == FILE_SLOT_RECEIVING,
        "admission: a refused retry leaves the transfer it would have replaced");
  for (uint16_t i = 0; i < chunksOf(a.size()); i++) rxChunk(kSender, 58, a, i);
  rxEndOf(kSender, 58, a);
  check(fake_vfs::fs().text(inboxPath(kSender, "retry.bin")) == a && admitRefreshes == 0,
        "admission: the surviving transfer completes; the RAM path never asks for a walk");
}

// FILE-3: the stored free figure is read again only by a log write, so files
// received one after the other are decided on the same reading. Each file a
// transfer has put on flash is counted against it (VFS::noteBulkWriteTaken)
// once the transfer is no longer in flight.
static void testStorageAdmissionCountsWrittenFiles() {
  resetWorld();
  const size_t n = 2000;
  fakeLittleFsFree = kFakeReserve + 3 * n;               // one reading, never refreshed in this test
  for (uint32_t id = 70; id < 73; id++) {
    const std::string bytes = pattern(n, static_cast<uint8_t>(id));
    char name[16];
    std::snprintf(name, sizeof(name), "f%lu.bin", static_cast<unsigned long>(id));
    check(isAccepted(rxStart(kSender, id, name, bytes.size())), "written files: a file that still fits is accepted");
    check(takenNotes.size() == id - 70, "written files: a transfer in flight is counted by its slot, not noted yet");
    for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, id, bytes, i);
    rxEndOf(kSender, id, bytes);
    check(fake_vfs::fs().text(inboxPath(kSender, name)) == bytes, "written files: and arrives");
    check(takenNotes.size() == id - 69 && takenNotes.back() == n, "written files: its bytes are counted when its slot is released");
  }
  check(isRefused(rxStart(kSender, 73, "f73.bin", 1), espnowanswer::kIoError) && fileSlotsActiveCount() == 0,
        "written files: the fourth is refused on the same reading; the reserve is kept");
  check(fsCallsUnderSlotLock == 0 && fsLockWaitsUnderSlotLock == 0, "written files: counting does no filesystem work");

  // A transfer that ends without all its chunks wrote nothing and is not counted.
  resetWorld();
  fakeLittleFsFree = kFakeReserve + 3 * n;
  const std::string part = pattern(n, 80);
  rxStart(kSender, 80, "part.bin", part.size());
  rxChunk(kSender, 80, part, 0);
  rxStop(kSender, 80);                                   // the sender gave up
  check(fileSlotsActiveCount() == 0 && takenNotes.empty() && results.empty(),
        "written files: an abandoned transfer is not counted (and gets no FILE_RESULT)");

  // A streamed file is counted write by write (its unwritten rest is in flight).
  resetWorld();
  const std::string big = pattern(kStreamSize, 81);
  fakeLittleFsFree = kFakeReserve + 2 * big.size();
  rxStart(kSender, 81, "big.bin", big.size());
  rxChunksWithWriter(kSender, 81, big, 0, chunksOf(big.size()));
  rxEndOf(kSender, 81, big);
  runCmdExec();
  check(fake_vfs::fs().text(inboxPath(kSender, "big.bin")) == big && fakeTakenBytes == big.size(),
        "written files: a streamed file is counted as it is written, once");
}

// FILE-3: a streaming transfer is asked again when cmd_exec opens its .part,
// with a fresh figure (the stored one may be old). Refused there, no staging
// file is made and the transfer fails like any stream write error.
static void testStorageAdmissionAtPartOpen() {
  resetWorld();
  const std::string bytes = pattern(kStreamSize, 11);
  const uint16_t n = chunksOf(bytes.size());
  check(isAccepted(rxStart(kSender, 61, "big.bin", bytes.size())) && admitPeeks == 1 &&
            slotState(61) == FILE_SLOT_RECEIVING,
        "part open: accepted on the stored figure");
  fakeLittleFsFree = kFakeReserve + bytes.size() - 1;   // what a fresh reading says
  rxChunksWithWriter(kSender, 61, bytes, 0, n);
  check(admitRefreshes == 1 && admitAsks.back() == bytes.size(), "part open: asked once more, for the whole file, with a fresh figure");
  check(admitRefreshesWithoutFsLock == 0 && admitRefreshesUnderSlotLock == 0 && admitPeeksUnderFsLock == 0,
        "part open: the fresh reading is taken on cmd_exec under the filesystem lock, never with the slot lock held");
  check(partFiles() == 0 && fake_vfs::fs().liveHandles() == 0, "part open: no staging file is created");
  rxEndOf(kSender, 61, bytes);
  runCmdExec();
  check(!fake_vfs::fs().exists(inboxPath(kSender, "big.bin")) && resultCount(61, espnowanswer::kOk) == 0,
        "part open: nothing is delivered or reported stored");
  check(fileSlotsActiveCount() == 0 && partFiles() == 0 && liveAllocations == 0, "part open: the slot and its buffers are released");
  // With room, the same transfer is asked once at the open and arrives.
  resetWorld();
  fakeLittleFsFree = kFakeReserve + bytes.size();
  rxStart(kSender, 62, "big.bin", bytes.size());
  rxChunksWithWriter(kSender, 62, bytes, 0, n);
  rxEndOf(kSender, 62, bytes);
  runCmdExec();
  check(admitPeeks == 1 && admitRefreshes == 1 && cancels.empty() &&
        fake_vfs::fs().text(inboxPath(kSender, "big.bin")) == bytes, "part open: a file that fits streams as before");
}

// Transfer A's teardown waits for the filesystem lock (a long directory walk
// holds it). Meanwhile chunks of transfer B arrive. B is healthy and must not
// be cancelled, and must complete.
static void testWriterBusyDoesNotCancelHealthyReceive() {
  resetWorld();
  const std::string a = pattern(kStreamSize, 3);
  const std::string b = pattern(900, 4);
  rxStart(kSender, 21, "a.bin", a.size());
  rxChunksWithWriter(kSender, 21, a, 0, 30);   // the .part is open by now
  rxStart(kOther, 22, "b.txt", b.size());
  rxChunk(kOther, 22, b, 0);
  rxStop(kSender, 21);                         // the sender aborted A: teardown is queued
  check(slotState(21) == FILE_SLOT_COMPLETING, "writer busy: A is waiting for its abort job");
  const unsigned cancelsBefore = static_cast<unsigned>(cancels.size());
  bool hookRan = false;
  onFsLockWait = [&] {
    hookRan = true;
    for (uint16_t i = 1; i < chunksOf(b.size()); i++) rxChunk(kOther, 22, b, i);
  };
  runCmdExec();                                // the abort job; B's chunks land while it waits
  check(hookRan, "writer busy: the abort job waited for the filesystem lock");
  check(cancels.size() == cancelsBefore, "a healthy receive is not cancelled while the writer is busy");
  check(fsLockWaitsUnderSlotLock == 0, "abort does not wait for the filesystem lock with the slot lock held");
  check(fsCallsUnderSlotLock == 0, "abort does no filesystem work (close, remove) with the slot lock held");
  rxEndOf(kOther, 22, b);
  check(fake_vfs::fs().text(inboxPath(kOther, "b.txt")) == b, "writer busy: B arrives byte for byte");
  check(cancelCount(22) == 0 && resultCount(22, espnowanswer::kOk) == 1, "writer busy: B is stored, never cancelled");
  check(slotState(21) == FILE_SLOT_FREE && partFiles() == 0, "writer busy: A's slot and staging file are gone");
  check(deadlocks == 0, "writer busy: no unbounded wait met a holder that was itself waiting");
}

// While a streaming slot's teardown is doing its filesystem work the slot stays
// claimed, so a new transfer cannot be given the same index (and with it the
// same staging path) and have its file removed by the teardown.
static void testTeardownKeepsTheSlotClaimed() {
  resetWorld();
  const std::string a = pattern(kStreamSize, 5);
  rxStart(kSender, 31, "a.bin", a.size());
  rxChunksWithWriter(kSender, 31, a, 0, 30);
  rxStop(kSender, 31);
  uint8_t stateDuringFsWork = FILE_SLOT_FREE;
  uint8_t slotsInUse = 0;
  bool retryAccepted = false;
  onFsLockWait = [&] {
    stateDuringFsWork = slotState(31);
    retryAccepted = isAccepted(rxStart(kSender, 32, "a.bin", a.size()));  // a retry of the same file
    slotsInUse = fileSlotsActiveCount();
  };
  runCmdExec();
  check(stateDuringFsWork == FILE_SLOT_COMPLETING, "teardown: the slot stays claimed during the filesystem work");
  check(slotsInUse == 2, "teardown: a retry that arrives meanwhile gets a different slot");
  check(retryAccepted, "teardown: the retry is accepted");
  check(slotState(31) == FILE_SLOT_FREE && slotState(32) == FILE_SLOT_RECEIVING,
        "teardown: the old slot is freed, the retry is receiving");
  rxChunksWithWriter(kSender, 32, a, 0, chunksOf(a.size()));
  rxEndOf(kSender, 32, a);
  runCmdExec();
  check(fake_vfs::fs().text(inboxPath(kSender, "a.bin")) == a, "teardown: the retry arrives byte for byte");
  check(cancelCount(32) == 0, "teardown: the retry is never cancelled");
}

// The slot table is busy for longer than the receive path waits. "Busy" is not
// "no such transfer": the frame is dropped, the sender is not told to abort,
// and the transfer is judged later by the code that knows its state.
static void testBusyTableIsNotAMissingTransfer() {
  resetWorld();
  const std::string bytes = pattern(900, 6);
  rxStart(kSender, 41, "ram.txt", bytes.size());
  rxChunk(kSender, 41, bytes, 0);
  slotLockHeldElsewhereMs = 80;
  rxChunk(kSender, 41, bytes, 1);              // dropped: the lookup timed out
  check(cancels.empty(), "busy lookup of a FILE_DATA frame sends no FILE_CANCEL");
  check(slotState(41) == FILE_SLOT_RECEIVING, "busy lookup leaves the transfer alone");
  slotLockHeldElsewhereMs = 0;
  for (uint16_t i = 2; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 41, bytes, i);
  rxEndOf(kSender, 41, bytes);
  check(cancels.empty() && results.size() == 1 && resultCount(41, espnowanswer::kFileIncomplete) == 1,
        "the dropped chunk is reported once, in FILE_RESULT, as incomplete (16)");
  check(eventCount(MSG_FILE_RECV_FAILED) == 1 && acks.size() == 1, "the FILE_END is taken; the receive is recorded as failed");
  check(fileSlotsActiveCount() == 0, "the incomplete transfer's slot is released");

  // A transfer that really is unknown still gets its cancel.
  resetWorld();
  rxChunk(kSender, 42, bytes, 0);
  check(cancels.size() == 1 && cancelCount(42, FILE_CANCEL_REJECTED) == 1,
        "FILE_DATA for an unknown transfer is still answered with FILE_CANCEL(REJECTED)");

  // Streaming: the lookup succeeds, the append finds the table busy.
  resetWorld();
  const std::string big = pattern(kStreamSize, 7);
  rxStart(kSender, 43, "big.bin", big.size());
  rxChunksWithWriter(kSender, 43, big, 0, 5);
  slotLockMissAtTake = 2;                      // take 1 = lookup, take 2 = append
  rxChunk(kSender, 43, big, 5);
  check(cancels.empty() && eventCount(MSG_FILE_RECV_FAILED) == 0,
        "busy append of a streaming chunk sends no FILE_CANCEL and records no failure");
  check(slotState(43) == FILE_SLOT_RECEIVING, "busy append leaves the streaming transfer receiving");
  rxChunk(kSender, 43, big, 6);                // the gap is found by the chunk after
  check(cancelCount(43, FILE_CANCEL_INCOMPLETE) == 1 && eventCount(MSG_FILE_RECV_FAILED) == 1,
        "the missing chunk is reported once, as a gap, by the next chunk");
  runCmdExec();
  check(fileSlotsActiveCount() == 0 && partFiles() == 0, "the failed streaming transfer is torn down");

  // FILE_END: one retry of the lookup, so a single miss does not lose the file.
  resetWorld();
  rxStart(kSender, 44, "end.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 44, bytes, i);
  slotLockMissAtTake = 1;
  rxEndOf(kSender, 44, bytes);
  check(fake_vfs::fs().text(inboxPath(kSender, "end.txt")) == bytes && acks.size() == 1 && cancels.empty() &&
            resultCount(44, espnowanswer::kOk) == 1,
        "FILE_END survives one busy lookup: the file is stored and reported stored");

  // FILE_END with the table busy for both tries: answered busy; the sweep
  // reports it, and a late FILE_END copy then gets the timeout verdict.
  resetWorld();
  rxStart(kSender, 45, "late.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 45, bytes, i);
  slotLockHeldElsewhereMs = 120;
  check(isRefused(rxEndOf(kSender, 45, bytes), espnowanswer::kBusy), "FILE_END on a busy table is answered busy");
  check(cancels.empty() && acks.empty() && results.empty(), "FILE_END on a busy table: no cancel, ack or result");
  check(slotState(45) == FILE_SLOT_RECEIVING, "FILE_END on a busy table leaves the slot for the sweep");
  slotLockHeldElsewhereMs = 0;
  FileSlotExpiry expired[4];
  nowMs += kFileSlotTimeoutMs + 1;
  check(fileSlotsTimeoutSweep(nowMs, expired, 4) == 1 && expired[0].transferId == 45,
        "the sweep reports the transfer whose FILE_END was dropped");
  check(fileSlotsActiveCount() == 0, "the sweep releases it");
  check(isAccepted(rxEndOf(kSender, 45, bytes)) && resultCount(45, espnowanswer::kTimeout) == 1,
        "a FILE_END that comes after the sweep gets FILE_RESULT timeout (remembered)");
}

// A streaming transfer fails; its abort job meets a slot lock that another task
// holds for longer than the old 50 ms wait. The job is the only thing that will
// ever free the slot, so it must not give up.
static void testAbortUnderContentionDoesNotPinTheSlot() {
  resetWorld();
  const std::string a = pattern(kStreamSize, 8);
  rxStart(kSender, 51, "a.bin", a.size());
  rxChunksWithWriter(kSender, 51, a, 0, 30);
  rxChunk(kSender, 51, a, 40);                 // a gap: the transfer is aborted
  check(slotState(51) == FILE_SLOT_COMPLETING && cancelCount(51, FILE_CANCEL_INCOMPLETE) == 1,
        "contended abort: the gap queued the teardown");
  slotLockHeldElsewhereMs = 200;
  runCmdExec();
  check(slotState(51) == FILE_SLOT_FREE, "abort under lock contention frees the slot");
  FileSlotExpiry expired[4];
  for (int tick = 0; tick < 3; tick++) {
    nowMs += kFileSlotTimeoutMs + 1;
    fileSlotsTimeoutSweep(nowMs, expired, 4);
    runCmdExec();
  }
  check(fileSlotsActiveCount() == 0, "abort under lock contention does not pin the slot");
  check(partFiles() == 0 && fake_vfs::fs().liveHandles() == 0, "contended abort: staging file removed, handle closed");
  check(liveAllocations == 0, "contended abort: both stream buffers are freed");
  check(fsCallsUnderSlotLock == 0 && fsLockWaitsUnderSlotLock == 0,
        "contended abort: no filesystem work with the slot lock held");
}

// The writer has put a buffer on flash; the slot lock is contended when it comes
// back to take the buffer out of the queue. Giving up there writes it twice.
static void testDrainUnderContentionWritesEachBufferOnce() {
  resetWorld();
  const std::string a = pattern(kStreamSize, 9);
  const uint16_t n = chunksOf(a.size());
  rxStart(kSender, 61, "a.bin", a.size());
  bool contended = false;
  onFsCall = [&](const fake_vfs::Call& call) {
    if (!contended && call.op == "write") {
      contended = true;
      slotLockHeldElsewhereMs = 200;           // espnow_task and others keep the lock busy
    }
  };
  rxChunksWithWriter(kSender, 61, a, 0, n);
  check(contended, "contended drain: a buffer was written");
  rxEndOf(kSender, 61, a);
  runCmdExec();
  check(cancelCount(61) == 0, "contended drain: the transfer is not cancelled");
  check(fake_vfs::fs().text(inboxPath(kSender, "a.bin")) == a,
        "drain under lock contention writes each buffer exactly once");
  check(fileSlotsActiveCount() == 0 && partFiles() == 0, "contended drain: slot and staging file are gone");
}

// FILE_END of a complete streaming file finds the command-task queue full for
// both the finalize and the abort job, so the teardown is parked for the sweep.
// Parking it must not be skipped on a contended lock.
static void testParkedTeardownUnderContention() {
  resetWorld();
  const std::string a = pattern(kStreamSize, 10);
  const uint16_t n = chunksOf(a.size());
  rxStart(kSender, 71, "a.bin", a.size());
  rxChunksWithWriter(kSender, 71, a, 0, n);
  // The park comes after two refused submits; the lock is contended from the
  // second one on.
  cmdExecRefuseNext = 2;
  onCmdExecRefused = [] { if (cmdExecRefuseNext == 0) slotLockHeldElsewhereMs = 200; };
  rxEndOf(kSender, 71, a);
  check(resultCount(71, espnowanswer::kIoError) == 1 && cancels.empty(),
        "parked teardown: the sender's FILE_RESULT says io_error");
  check(slotState(71) == FILE_SLOT_COMPLETING, "parked teardown: the slot waits for the sweep");
  runCmdExec();
  check(slotState(71) == FILE_SLOT_COMPLETING, "parked teardown: nothing was queued yet");
  check(fileSlotsTimeoutSweep(nowMs, nullptr, 0) == 0, "parked teardown: the retry is not an expiry");
  runCmdExec();
  check(fileSlotsActiveCount() == 0 && partFiles() == 0, "parked teardown: the sweep's retry frees the slot");
  check(fsCallsUnderSlotLock == 0 && fsLockWaitsUnderSlotLock == 0,
        "parked teardown: no filesystem work with the slot lock held");
}

// =====================================================================================================
// Send side
// =====================================================================================================
static const char* const kFile = "/data/report.bin";

static void putSendFile(const std::string& bytes) { fake_vfs::fs().put(kFile, bytes); }
// The receiving device's command task runs while the sender paces.
static void receiverKeepsUp() { onTaskDelay = [] { runCmdExec(); }; }
static unsigned airCount(uint8_t type) {
  unsigned n = 0;
  for (const auto& f : air) if (f.type == type) ++n;
  return n;
}
static uint32_t transferIdOnAir(size_t i) {
  uint32_t id = 0;
  if (i < air.size() && air[i].payload.size() >= 4) std::memcpy(&id, air[i].payload.data(), 4);
  return id;
}

static void testSendWithSessionArrives() {
  resetWorld();
  const std::string bytes = pattern(1000, 20);
  putSendFile(bytes);
  loopback = true;
  receiverKeepsUp();
  const FileSendOutcome out = sendFileToMacResult(kReceiver, String(kFile));
  check(out.result == FileSendResult::Stored && out.bytesStored == bytes.size(),
        "send with a session: Stored, with the receiver's byte count");
  check(air.size() == 2u + chunksOf(bytes.size()) && air.front().type == ESPNOW_V4_TYPE_FILE_START &&
            air.back().type == ESPNOW_V4_TYPE_FILE_END,
        "send with a session: START, every chunk and END left the sender");
  bool ownIds = true, oneTransfer = true;
  for (size_t i = 0; i < air.size(); i++) {
    for (size_t j = i + 1; j < air.size(); j++) ownIds = ownIds && air[i].msgId != air[j].msgId;
    oneTransfer = oneTransfer && transferIdOnAir(i) == transferIdOnAir(0);
  }
  check(ownIds && oneTransfer && transferIdOnAir(0) != 0,
        "every frame has its own msgId; all carry one nonzero transferId");
  check((air.front().flags & ESPNOW_V4_FLAG_ACK_REQ) && (air.back().flags & ESPNOW_V4_FLAG_ACK_REQ) &&
            (air[1].flags & ESPNOW_V4_FLAG_ACK_REQ) == 0,
        "FILE_START and FILE_END ask for an ACK; FILE_DATA does not");
  check(fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes,
        "send with a session: the receiver has the file byte for byte");
  check(sessionOpenKicks == 0 && keyExKicks == 0 && !pendingInUse(), "send with a session: no handshake, nothing parked");
  check(cancels.empty() && !gEspNow->fileSendInProgress, "send with a session: no cancel, the send flag is cleared");
  check(sendFileToMac(kReceiver, String(kFile)), "sendFileToMac (bool) is true for a stored file");

  // A file large enough to stream on the receiver.
  resetWorld();
  const std::string big = pattern(kStreamSize, 21);
  putSendFile(big);
  loopback = true;
  receiverKeepsUp();
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::Stored,
        "streaming send: Stored once the finalize job renamed it");
  check(fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == big,
        "streaming send: the receiver has the file byte for byte");
  check(cancels.empty() && fileSlotsActiveCount() == 0, "streaming send: no cancel, the receive slot is freed");

  // To a peer behind a relay the chunk is 138 bytes (DESIGN 3.6).
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  routedPeer = true;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::Stored &&
            airCount(ESPNOW_V4_TYPE_FILE_DATA) == (bytes.size() + 137) / 138 &&
            air[1].payload.size() == ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT,
        "routed send: 138-byte chunks, each frame within the 146-byte routed budget");
  check(fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes, "routed send: arrives byte for byte");
}

static void testSendWithoutSessionIsRefused() {
  resetWorld();
  const std::string bytes = pattern(1000, 22);
  putSendFile(bytes);
  loopback = true;
  receiverKeepsUp();
  receiverSession.state = SESSION_FREE;
  const uint32_t before = nowMs;
  const FileSendResult result = sendFileToMacResult(kReceiver, String(kFile)).result;
  // The pump has returned: the command task is free and finishes the handshake.
  commandTaskCompletesHandshake();
  runCmdExec();
  check(result == FileSendResult::NoSession, "a send without an ACTIVE session is refused as NoSession");
  check(air.empty() && pendingParked == 0, "a refused send puts no frame on air and none in the pending store");
  check(sessionOpenKicks == 1, "a refused send starts the session handshake once");
  check(nowMs == before, "a refused send does not wait on the command task");
  check(!gEspNow->fileSendInProgress && fake_vfs::fs().liveHandles() == 0, "a refused send leaves no send flag and no open file");

  // The retry the reply asks for.
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::Stored, "the retry after the handshake is stored");
  check(fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes, "the retry arrives byte for byte");

  // A handshake or a rekey that is already in flight is left alone.
  resetWorld();
  putSendFile(bytes);
  receiverSession.state = SESSION_ESTABLISHING;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::NoSession, "handshake in flight: refused");
  check(sessionOpenKicks == 0 && receiverSession.state == SESSION_ESTABLISHING, "handshake in flight: not restarted");
  receiverSession.state = SESSION_REKEYING;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::NoSession, "rekey in flight: refused");
  check(sessionOpenKicks == 0 && receiverSession.state == SESSION_REKEYING, "rekey in flight: the working session is not reset");
  check(air.empty() && !pendingInUse(), "in-flight handshake: no frame on air or parked");

  // No session slot at all, and a closed one: each is kicked once.
  receiverHasSessionSlot = false;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::NoSession && sessionOpenKicks == 1,
        "no session slot: refused, handshake started");
  receiverSession.state = SESSION_CLOSED;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::NoSession && sessionOpenKicks == 2,
        "closed session: refused, handshake started");

  // No identity yet: the key exchange is started once.
  resetWorld();
  putSendFile(bytes);
  receiverHasIdentity = false;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::NoSession && keyExKicks == 1 &&
            sessionOpenKicks == 0,
        "no peer identity: refused, key exchange started");
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::NoSession && keyExKicks == 1,
        "no peer identity: a key exchange in flight is not started again");
  check(air.empty() && !pendingInUse(), "no peer identity: no frame on air or parked");

  // A handshake that cannot be started is a failure, not "try again".
  resetWorld();
  putSendFile(bytes);
  receiverHasSessionSlot = false;
  kicksFail = true;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::Failed && sessionOpenKicks == 1,
        "SESSION_OPEN cannot be started: Failed, not NoSession");
  receiverHasIdentity = false;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::Failed && keyExKicks == 1,
        "key exchange cannot be started: Failed, not NoSession");
  check(air.empty() && !pendingInUse(), "handshake cannot start: no frame on air or parked");

  // The bool form, which the FS_GET server and automations.json use.
  resetWorld();
  putSendFile(bytes);
  receiverSession.state = SESSION_FREE;
  const bool boolResult = sendFileToMac(kReceiver, String(kFile));
  commandTaskCompletesHandshake();
  check(!boolResult, "sendFileToMac (bool) is false for a send without a session");

  // A file problem is reported as such, and starts no handshake.
  resetWorld();
  receiverSession.state = SESSION_FREE;
  const FileSendOutcome missing = sendFileToMacResult(kReceiver, String("/data/missing.bin"));
  check(missing.result == FileSendResult::Failed && missing.status == espnowanswer::kNotFound && sessionOpenKicks == 0,
        "a missing file is a failure (not_found), not a session refusal, and starts no handshake");
  putSendFile(bytes);
  readAllowed = false;
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::Failed && sessionOpenKicks == 0,
        "a file the caller may not read is a failure and starts no handshake");
}

static void testSessionLostMidTransfer() {
  resetWorld();
  const std::string bytes = pattern(4000, 23);
  putSendFile(bytes);
  loopback = true;
  receiverKeepsUp();
  unsigned dataFrames = 0;
  onAirFrame = [&](const AirFrame& f) {
    if (f.type == ESPNOW_V4_TYPE_FILE_DATA && ++dataFrames == 5) receiverSession.state = SESSION_CLOSED;
  };
  const FileSendOutcome out = sendFileToMacResult(kReceiver, String(kFile));
  commandTaskCompletesHandshake();
  runCmdExec();
  check(out.result == FileSendResult::Failed && std::strstr(out.why, "session") != nullptr,
        "a session lost mid-transfer is a failed send, and says why");
  check(!fake_vfs::fs().exists(inboxPath(kSender, "report.bin")), "nothing was stored");
  check(dataFrames == 5 && pendingParked == 0, "after the session is lost no further chunk is parked");
  check(sessionOpenKicks == 0, "the pump does not restart the session itself");
  check(!gEspNow->fileSendInProgress && fake_vfs::fs().liveHandles() == 0, "the aborted send closes its file and clears its flag");
  // The receiver's slot is left to its timeout sweep.
  FileSlotExpiry expired[4];
  nowMs += kFileSlotTimeoutMs + 1;
  check(fileSlotsTimeoutSweep(nowMs, expired, 4) == 1, "the receiver's sweep reclaims the abandoned transfer");
}

// The three results of the step 7 row: Stored (above), Failed (the receiver
// could not store it), Unknown (no FILE_RESULT came), and the paths between.
static void testSendFailedAndUnknown() {
  const std::string bytes = pattern(1000, 25);

  // Failed: the receiver's rename fails, its FILE_RESULT says io_error.
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  fake_vfs::fs().faults.rename.add("*");
  FileSendOutcome out = sendFileToMacResult(kReceiver, String(kFile));
  check(out.result == FileSendResult::Failed && out.status == espnowanswer::kIoError &&
            std::strcmp(out.why, "io_error") == 0,
        "the receiver could not write the file: Failed(io_error)");
  fake_vfs::fs().faults.clear();

  // Unknown: no FILE_RESULT reaches the sender. It asks once more (a FILE_END
  // copy under a new msgId, 4 s in) and gives up after 10 s.
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  dropResults = -1;
  uint32_t before = nowMs;
  out = sendFileToMacResult(kReceiver, String(kFile));
  const uint32_t waited = nowMs - before;
  check(out.result == FileSendResult::Unknown, "no FILE_RESULT: Unknown");
  check(waited >= kFileResultWaitMs && waited < kFileResultWaitMs + 2000, "Unknown after the 10 s result wait");
  check(airCount(ESPNOW_V4_TYPE_FILE_END) == 2 && air.back().msgId != air[air.size() - 2].msgId &&
            resultCount(transferIdOnAir(0), espnowanswer::kOk) == 2,
        "the sender asks again once (a FILE_END copy, own msgId); the receiver answers from its memory");
  check(fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes, "Unknown: the file was in fact stored");
  // The result that arrives after the send gave up is shown as late, once.
  dropResults = 0;
  const uint32_t id = transferIdOnAir(0);
  v4_send_file_result(kSender, id, espnowanswer::kOk, 1000);
  v4_send_file_result(kSender, id, espnowanswer::kOk, 1000);
  unsigned late = 0;
  for (const auto& b : broadcasts) late += b.find("late result") != std::string::npos;
  check(late == 1, "a FILE_RESULT after Unknown is shown once as a late result");

  // The first FILE_RESULT is lost, the answer to the copy arrives: Stored.
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  dropResults = 1;
  before = nowMs;
  out = sendFileToMacResult(kReceiver, String(kFile));
  check(out.result == FileSendResult::Stored && nowMs - before >= kFileResultAskAgainMs &&
            nowMs - before < kFileResultWaitMs,
        "a lost FILE_RESULT: the copy of FILE_END brings it again, Stored");

  // FILE_END's ACK is lost but the receiver took it: the result still comes.
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  onAirFrame = [](const AirFrame& f) { if (f.type == ESPNOW_V4_TYPE_FILE_END) lostAcks = 1; };
  out = sendFileToMacResult(kReceiver, String(kFile));
  check(out.result == FileSendResult::Stored, "a lost FILE_END ACK: the FILE_RESULT still decides, Stored");
}

// A chunk changed on the way: the receiver's whole-file CRC32 refuses it.
static void testSendBadCrc() {
  resetWorld();
  const std::string bytes = pattern(1000, 26);
  putSendFile(bytes);
  loopback = true;
  unsigned dataSeen = 0;
  mutateOnAir = [&](AirFrame& f) {
    if (f.type == ESPNOW_V4_TYPE_FILE_DATA && ++dataSeen == 3) f.payload.back() ^= 0x5A;
  };
  const FileSendOutcome out = sendFileToMacResult(kReceiver, String(kFile));
  check(out.result == FileSendResult::Failed && out.status == espnowanswer::kFileCrcMismatch &&
            std::strcmp(out.why, "crc_mismatch") == 0,
        "bad CRC: Failed(crc_mismatch, 17) from the receiver's FILE_RESULT");
  check(!fake_vfs::fs().exists(inboxPath(kSender, "report.bin")) && fileSlotsActiveCount() == 0,
        "bad CRC: nothing stored, the slot released");
}

// FILE_CANCEL mid-pump, both directions.
static void testCancelMidPump() {
  // The receiver: a streaming chunk comes out of order (chunk 4 says it is
  // chunk 9), so the receiver sees the gap; its FILE_CANCEL(incomplete) stops
  // the sender within one chunk.
  const std::string big = pattern(kStreamSize, 27);
  unsigned dataSent = 0;
  resetWorld();
  putSendFile(big);
  loopback = true;
  receiverKeepsUp();
  unsigned dataAfterCancel = 0;
  mutateOnAir = [&](AirFrame& f) {
    if (f.type != ESPNOW_V4_TYPE_FILE_DATA) return;
    ++dataSent;
    if (!cancels.empty()) ++dataAfterCancel;
    if (dataSent == 4) { uint32_t ahead = 9; std::memcpy(f.payload.data() + 4, &ahead, 4); }  // a gap
  };
  const FileSendOutcome cancelled = sendFileToMacResult(kReceiver, String(kFile));
  check(cancelled.result == FileSendResult::Failed && cancelled.status == espnowanswer::kFileIncomplete,
        "cancel mid-pump: the receiver's FILE_CANCEL(incomplete) ends the send as Failed(incomplete)");
  check(!cancels.empty() && dataAfterCancel <= 1, "cancel mid-pump: the pump stops within one chunk of the cancel");
  check(airCount(ESPNOW_V4_TYPE_FILE_END) == 0, "cancel mid-pump: no FILE_END to a receiver that already stopped");
  runCmdExec();
  check(fileSlotsActiveCount() == 0 && partFiles() == 0 && liveAllocations == 0,
        "cancel mid-pump: the receiver's slot and buffers are freed");
  check(!fake_vfs::fs().exists(inboxPath(kSender, "report.bin")), "cancel mid-pump: nothing stored");

  // The sender: FILE_START's ACK is lost although the receiver took it; the
  // sender gives up at once and its FILE_CANCEL frees the receiver's slot.
  resetWorld();
  const std::string bytes = pattern(1000, 28);
  putSendFile(bytes);
  loopback = true;
  lostAcks = 1;
  const FileSendOutcome noAnswer = sendFileToMacResult(kReceiver, String(kFile));
  check(noAnswer.result == FileSendResult::Failed && noAnswer.status == espnowanswer::kTimeout,
        "no answer to FILE_START: Failed(timeout) at once");
  check(air.size() == 2 && air[0].type == ESPNOW_V4_TYPE_FILE_START && air[1].type == ESPNOW_V4_TYPE_FILE_CANCEL &&
            transferIdOnAir(1) == transferIdOnAir(0),
        "no answer to FILE_START: no chunk, a FILE_CANCEL for the transferId");
  check(fileSlotsActiveCount() == 0 && eventCount(MSG_FILE_RECV_FAILED) == 1,
        "the sender's FILE_CANCEL frees the receiver's slot at once");
  // The same cancel for a transfer the receiver never had frees nothing and is
  // not taken for a failed send of the receiver's own.
  check(isAccepted(rxFrame(ESPNOW_V4_TYPE_FILE_CANCEL, kSender, ++nextFrameMsgId, air[1].payload.data(),
                           static_cast<uint16_t>(air[1].payload.size()))) &&
            eventCount(MSG_FILE_SEND_FAILED) == 0,
        "a sender's cancel for an unknown transfer is taken quietly (no failed-send record)");
}

// F7 from the sender: a long name goes whole and the receiver refuses it; a
// name that cannot fit FILE_START at all is refused before any frame.
static void testNameTooLong() {
  resetWorld();
  const std::string bytes = pattern(500, 29);
  const std::string longName(97, 'n');
  fake_vfs::fs().put("/data/" + longName, bytes);
  loopback = true;
  FileSendOutcome out = sendFileToMacResult(kReceiver, String(("/data/" + longName).c_str()));
  check(out.result == FileSendResult::Failed && out.status == espnowanswer::kTooLarge &&
            std::strcmp(out.why, "too_large") == 0,
        "a 97-byte name: Failed(too_large) from the receiver's ACK");
  check(air.size() == 1 && air[0].type == ESPNOW_V4_TYPE_FILE_START &&
            air[0].payload.size() == sizeof(V4PayloadFileStart) + longName.size(),
        "the name went whole (never cut) and no chunk followed");
  check(fileSlotsActiveCount() == 0, "the receiver holds no slot");
  // The command says so.
  std::string reply = cmd_espnow_sendfile(String(("peerB \"/data/" + longName + "\"").c_str()));
  check(reply == "Error: '" + longName + "' was not stored on peerB: too_large.",
        "espnowsendfile: 'Error: ... was not stored on peerB: too_large.'");
  // 191 bytes cannot fit a 202-byte FILE_START: refused here, nothing sent.
  resetWorld();
  const std::string huge(191, 'h');
  fake_vfs::fs().put("/data/" + huge, bytes);
  loopback = true;
  out = sendFileToMacResult(kReceiver, String(("/data/" + huge).c_str()));
  check(out.result == FileSendResult::Failed && out.status == espnowanswer::kTooLarge && air.empty(),
        "a name that cannot fit FILE_START: Failed(too_large) before any frame");
}

// FILE_START refused: the send ends at once — one frame on the air, no wait,
// no retry.
static void testStartRefusedFast() {
  resetWorld();
  const std::string bytes = pattern(800, 40);
  putSendFile(bytes);
  for (uint32_t id = 901; id <= 904; id++) {  // every slot taken
    rxStartNamed(kOther, id, "fill" + std::to_string(id) + ".bin", 100);
  }
  check(fileSlotsActiveCount() == 4, "four slots in use");
  loopback = true;
  const uint32_t before = nowMs;
  const FileSendOutcome out = sendFileToMacResult(kReceiver, String(kFile));
  check(out.result == FileSendResult::Failed && out.status == espnowanswer::kNoSlot &&
            std::strcmp(out.why, "no_slot") == 0,
        "START refused: Failed(no_slot)");
  check(air.size() == 1 && ackedSends == 1 && nowMs == before,
        "START refused: one frame, no retry, no chunk, no wait");
  check(!gEspNow->fileSendInProgress && fake_vfs::fs().liveHandles() == 0, "START refused: flag cleared, file closed");
  for (uint32_t id = 901; id <= 904; id++) rxStop(kOther, id);
}

static void testCallersReportTheResult() {
  const std::string bytes = pattern(1000, 24);

  // espnowsendfile (also what a remote espnowfetch runs and gets back).
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  receiverKeepsUp();
  receiverSession.state = SESSION_FREE;
  std::string reply = cmd_espnow_sendfile(String("peerB \"/data/report.bin\""));
  commandTaskCompletesHandshake();
  runCmdExec();
  check(reply == kNoSessionPeerB, "espnowsendfile without a session: the no-session reply");
  check(reply.rfind("Error:", 0) == 0 && reply.find("Stored") == std::string::npos,
        "espnowsendfile without a session: an error the web card cannot read as stored");
  check(gEspNow->fileTransfersSent == 0 && eventCount(MSG_FILE_SEND_SUCCESS) == 0,
        "espnowsendfile without a session: nothing counted or recorded as sent");
  reply = cmd_espnow_sendfile(String("peerB \"/data/report.bin\""));
  check(reply == "Stored on peerB: report.bin (1000 bytes)", "espnowsendfile: 'Stored on peerB: report.bin (1000 bytes)'");
  check(gEspNow->fileTransfersSent == 1 && fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes,
        "espnowsendfile stored: counted, and the file arrived");
  dropResults = -1;
  reply = cmd_espnow_sendfile(String("peerB \"/data/report.bin\""));
  check(reply.rfind("Unknown: every part of 'report.bin' went to peerB", 0) == 0 && gEspNow->fileTransfersSent == 1,
        "espnowsendfile with no FILE_RESULT: 'Unknown: ...', not counted as sent");
  dropResults = 0;

  // imagesend.
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  receiverKeepsUp();
  receiverSession.state = SESSION_FREE;
  reply = cmd_imagesend(String("peerB \"/data/report.bin\""));
  commandTaskCompletesHandshake();
  runCmdExec();
  check(reply == kNoSessionPeerB, "imagesend without a session: the no-session reply");
  check(hints.empty(), "imagesend without a session: no 'in the inbox' hint");
  reply = cmd_imagesend(String("peerB \"/data/report.bin\""));
  check(reply == "Stored /data/report.bin on peerB (1000 bytes)" && hints.size() == 1, "imagesend: 'Stored ... on peerB'");
  fake_vfs::fs().faults.rename.add("*");
  reply = cmd_imagesend(String("peerB \"/data/report.bin\""));
  check(reply == "Error: the image was not stored on peerB: io_error.", "imagesend: the receiver's reason on a failure");
  fake_vfs::fs().faults.clear();

  // The OLED send job.
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  receiverKeepsUp();
  receiverSession.state = SESSION_FREE;
  auto job = [] {
    auto* j = static_cast<EspNowSendFileJob*>(std::malloc(sizeof(EspNowSendFileJob)));
    std::memcpy(j->mac, kReceiver, 6);
    std::snprintf(j->path, sizeof(j->path), "%s", kFile);
    return j;
  };
  runEspNowSendFile(job());
  commandTaskCompletesHandshake();
  runCmdExec();
  check(toasts.size() == 1 && toasts[0] == "No session yet. Try again.", "OLED send without a session: says so");
  runEspNowSendFile(job());
  check(toasts.size() == 2 && toasts[1] == "File stored", "OLED send: 'File stored'");
  fake_vfs::fs().faults.rename.add("*");
  runEspNowSendFile(job());
  check(toasts.size() == 3 && toasts[2] == "Not stored: io_error", "OLED send: 'Not stored: <reason>'");
  fake_vfs::fs().faults.clear();
  dropResults = -1;
  runEspNowSendFile(job());
  check(toasts.size() == 4 && toasts[3] == "Sent, no answer. Check peer.", "OLED send with no FILE_RESULT: says so");
  dropResults = 0;

  // espnowsend with a message too long for one frame.
  const std::string longText(300, 'x');
  resetWorld();
  receiverSession.state = SESSION_FREE;
  uint32_t before = nowMs;
  reply = cmd_espnow_send(String(("peerB " + longText).c_str()));
  check(reply == kNoSessionPeerB, "espnowsend (multi-frame) without a session: the no-session reply");
  check(nowMs == before, "espnowsend (multi-frame) without a session does not block the command task");
  check(sessionOpenKicks == 1 && receiverSession.state == SESSION_ESTABLISHING, "espnowsend (multi-frame): the handshake is started once");
  check(aeadSyncCalls == 0 && statusRegistered == 0 && historyStored == 0,
        "espnowsend (multi-frame) without a session: nothing sent, tracked or stored as sent");
  reply = cmd_espnow_send(String(("peerB " + longText).c_str()));
  check(reply == kNoSessionPeerB && sessionOpenKicks == 1, "espnowsend (multi-frame): a handshake in flight is not restarted");
  reply = cmd_espnow_send(String(("json peerB " + longText).c_str()));
  check(reply == "{\"schema\":1,\"ok\":false,\"error\":\"no session\"}", "espnowsend json (multi-frame) without a session: error object");
  commandTaskCompletesHandshake();
  reply = cmd_espnow_send(String(("peerB " + longText).c_str()));
  check(reply.rfind("Encrypted message sent via SESSION_FRAME", 0) == 0 && aeadSyncCalls == 1 && historyStored == 1,
        "espnowsend (multi-frame) with a session: sent as before");
  check(statusReads == 1 && deliveryStamps == 0,
        "espnowsend: the status is re-read after the sent record, and a pending one is not stamped");
  statusState = SEND_STATUS_DELIVERED;
  reply = cmd_espnow_send(String(("peerB " + longText).c_str()));
  check(reply.rfind("Encrypted message sent via SESSION_FRAME", 0) == 0 && historyStored == 2 && statusReads == 2,
        "espnowsend (multi-frame, delivered during the send): sent");
  check(deliveryStamps == 1 && historyStoredAtStamp == 2 && stampMsgId == lastRegisteredMsgId && stampMsgId != 0 &&
            stampState == SEND_STATUS_DELIVERED && std::memcmp(stampMac, kReceiver, 6) == 0,
        "espnowsend: a state resolved during the send is stamped on the stored record (peer, msgId, delivered)");

  // One frame still rides the pending store across the handshake.
  resetWorld();
  receiverSession.state = SESSION_FREE;
  before = nowMs;
  reply = cmd_espnow_send(String("peerB hello"));
  check(reply.rfind("Encrypted message queued until the session with peerB opens (ID: ", 0) == 0 &&
            reply.find(", 5 plaintext bytes)") != std::string::npos && pendingInUse() && nowMs == before,
        "espnowsend (one frame) without a session: parked for the handshake and reported queued");
  check(historyStored == 1 && statusFailed == 0, "espnowsend (queued): stored in the history as a pending send");
  commandTaskCompletesHandshake();
  check(air.size() == 1 && air[0].type == ESPNOW_V4_TYPE_TEXT, "espnowsend (one frame): delivered when the session opens");
  reply = cmd_espnow_send(String("peerB hello"));
  check(reply.rfind("Encrypted message sent via SESSION_FRAME", 0) == 0 && air.size() == 2,
        "espnowsend with a session: reported sent");

  // Up to four espnowsend in a row before the session opened are each kept.
  resetWorld();
  receiverSession.state = SESSION_FREE;
  std::vector<uint32_t> ids;
  for (int i = 0; i < 4; ++i) {
    reply = cmd_espnow_send(String(("peerB m" + std::to_string(i)).c_str()));
    check(reply.rfind("Encrypted message queued until the session with peerB opens", 0) == 0,
          "espnowsend in a row before the session: each reply says queued");
    ids.push_back(lastRegisteredMsgId);
  }
  check(pendingFrames.size() == 4 && sessionOpenKicks == 1, "four sends wait together; one handshake is started");
  reply = cmd_espnow_send(String("peerB m4"));
  check(reply == "Error: 4 messages to peerB already wait for the session to open. Retry in a few seconds.",
        "the fifth send while four wait: refused, says why and what to do");
  check(statusFailed == 1 && historyStored == 4 && pendingFrames.size() == 4,
        "the refused send is marked failed and not stored as sent; the four waiting stay");
  reply = cmd_espnow_send(String("json peerB m5"));
  check(reply == "{\"schema\":1,\"ok\":false,\"error\":\"waiting queue full\"}", "json: the refusal names the full queue");
  commandTaskCompletesHandshake();
  bool inOrder = air.size() == 4;
  for (size_t i = 0; inOrder && i < 4; ++i) inOrder = air[i].msgId == ids[i];
  check(inOrder, "the four queued messages go out in the order they were sent");
  resetWorld();
  receiverSession.state = SESSION_FREE;
  reply = cmd_espnow_send(String("json peerB hi"));
  check(reply.find("\"ok\":true") != std::string::npos && reply.find(",\"queued\":true}") != std::string::npos,
        "json: a queued send says queued");
}

// Bench 2026-11-18: the bond's own sync files raised a file_rx event each
// time. Only these names, only from the bond partner, only with bond mode on.
static void testBondSyncFilesAreQuiet() {
  for (const char* name : {"_settings_out.json", "_manifest_out.json", "_schema_out.json", "automations.json"}) {
    check(fileRxIsBondSync(true, true, name), "a bond sync file from the partner is quiet");
    check(!fileRxIsBondSync(true, false, name), "the same name from another device is a received file");
    check(!fileRxIsBondSync(false, true, name), "with bond mode off it is a received file");
  }
  for (const char* name : {"notes.txt", "settings_out.json", "_settings_out.json.bak", "automations.jsonx",
                           "Automations.json", "", "a/_settings_out.json"}) {
    check(!fileRxIsBondSync(true, true, name), "a file a person sends from the partner raises its event");
  }
  check(!fileRxIsBondSync(true, true, nullptr), "no name: not quiet");
}

// Rule F3: FILE_CANCEL is an answer. reqId = the transferId, status = the
// reason, under a fresh header msgId of the canceller's own; the sender stops
// the transfer the reqId names and reports it once.
static void testCancelIsAnAnswer() {
  resetWorld();
  loopback = true;
  gEspNow->fileSendInProgress = true;
  std::memcpy(gEspNow->fileSendPeer, kReceiver, 6);
  gEspNow->fileSendTransferId = 777;
  v4_send_file_cancel(kSender, 777, FILE_CANCEL_INCOMPLETE);
  check(cancels.size() == 1 && cancels[0].id == 777 && cancels[0].reason == espnowanswer::kFileIncomplete,
        "cancel answer: reqId = the transferId, status incomplete (16)");
  check(borrowedCancelIds == 0 && cancels[0].frameMsgId != 777,
        "cancel answer: its header msgId is the canceller's own, not the transferId");
  check(gEspNow->fileSendAbortTransferId == 777 && gEspNow->fileSendAbortStatus == espnowanswer::kFileIncomplete,
        "cancel answer: the sender stops the transfer the reqId names, with the reason");
  check(eventCount(MSG_FILE_SEND_FAILED) == 1, "cancel answer: the failed send is recorded");
  v4_send_file_cancel(kSender, 777, FILE_CANCEL_INCOMPLETE);
  check(cancels.size() == 2 && cancels[1].frameMsgId != cancels[0].frameMsgId,
        "cancel answer: each repeat has a header msgId of its own");
  check(eventCount(MSG_FILE_SEND_FAILED) == 1, "cancel answer: a repeat is not recorded twice");
  gEspNow->fileSendAbortTransferId = 0;
  nowMs += 6000;
  v4_send_file_cancel(kSender, 778, FILE_CANCEL_CRC_MISMATCH);
  check(gEspNow->fileSendAbortTransferId == 0, "cancel answer: another transfer's cancel stops nothing");
  // FILE_RESULT is an answer the same way.
  gEspNow->fileSendTransferId = 779;
  v4_send_file_result(kSender, 779, espnowanswer::kOk, 42);
  check(gEspNow->fileSendResultTransferId == 779 && gEspNow->fileSendResultStatus == 0 &&
            gEspNow->fileSendResultBytes == 42,
        "result answer: published for the send it names");
  v4_send_file_result(kSender, 780, espnowanswer::kOk, 42);
  check(gEspNow->fileSendResultTransferId == 779, "result answer: another transfer's result is not taken for this one");
  gEspNow->fileSendInProgress = false;
}

// 1.0 step 5b: heavy work leaves the ACK path. A small file's FILE_END is
// accepted (its ACK goes out) once the file is complete and its CRC checked;
// the flash write runs on the espnow_task loop's next pass, and FILE_RESULT
// follows it (step 7).
static void testRamFinishAfterAck() {
  resetWorld();
  const std::string bytes = pattern(1000, 3);
  rxStart(kSender, 15, "later.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 15, bytes, i);
  gHoldLoopPass = true;
  rxEndOf(kSender, 15, bytes);
  check(acks.size() == 1 && acks[0] == 15 && !fake_vfs::fs().exists(inboxPath(kSender, "later.txt")) && results.empty(),
        "RAM path: FILE_END is accepted before the file is written; no FILE_RESULT yet");
  check(slotState(15) == FILE_SLOT_COMPLETING, "RAM path: the accepted slot leaves RECEIVING");
  rxEndOf(kSender, 15, bytes);
  rxChunk(kSender, 15, bytes, 0);
  check(acks.size() == 2 && cancels.empty() && results.empty() && slotState(15) == FILE_SLOT_COMPLETING,
        "RAM path: a copy of the FILE_END is answered as taken and a late chunk changes nothing");
  gHoldLoopPass = false;
  fileRamFinalizeTick();
  check(fake_vfs::fs().text(inboxPath(kSender, "later.txt")) == bytes && fileSlotsActiveCount() == 0 &&
            eventCount(MSG_FILE_RECV_SUCCESS) == 1 && resultCount(15, espnowanswer::kOk) == 1,
        "RAM path: the loop's next pass writes the file once, sends FILE_RESULT stored and releases the slot");
  // ESP-NOW closes between the ACK and the loop pass: the accepted file is
  // reported failed (FILE_RESULT io_error), not lost silently.
  resetWorld();
  rxStart(kSender, 18, "closed.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 18, bytes, i);
  gHoldLoopPass = true;
  rxEndOf(kSender, 18, bytes);
  gHoldLoopPass = false;
  fileRamFinalizeDropAll();
  check(acks.size() == 1 && resultCount(18, espnowanswer::kIoError) == 1 && cancels.empty() &&
            eventCount(MSG_FILE_RECV_FAILED) == 1 && fileSlotsActiveCount() == 0 &&
            !fake_vfs::fs().exists(inboxPath(kSender, "closed.txt")),
        "RAM path: closing ESP-NOW after the ACK reports the file failed (FILE_RESULT io_error) and frees the slot");
  // A CRC mismatch: the FILE_END is taken, the FILE_RESULT says crc_mismatch.
  resetWorld();
  rxStart(kSender, 16, "bad.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 16, bytes, i);
  check(isAccepted(rxEnd(kSender, 16, crcOf(bytes) ^ 1u, chunksOf(bytes.size()))) &&
            resultCount(16, espnowanswer::kFileCrcMismatch) == 1 && cancels.empty() &&
            !fake_vfs::fs().exists(inboxPath(kSender, "bad.txt")),
        "RAM path: a CRC mismatch is taken at FILE_END and answered FILE_RESULT crc_mismatch (17)");
  // A chunk count other than FILE_START implied: incomplete.
  rxStart(kSender, 19, "count.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 19, bytes, i);
  rxEnd(kSender, 19, crcOf(bytes), chunksOf(bytes.size()) + 1u);
  check(resultCount(19, espnowanswer::kFileIncomplete) == 1 && fileSlotsActiveCount() == 0,
        "FILE_END with another chunk count than FILE_START implied: FILE_RESULT incomplete");
}

// Review fixes (2026-11-23): a chunk must be exactly chunkSize (the last one
// the rest of totalSize); a late chunk of a finished transfer draws no cancel;
// cancels for unknown transfers are limited per peer.
static void testChunkLengthAndLateChunks() {
  // Streaming: FILE_START declares 3-byte chunks, the peer sends 194 bytes.
  resetWorld();
  check(isAccepted(rxStartNamed(kSender, 81, "big.bin", kStreamSize, V4_FILE_KIND_FILE, 3)),
        "a 3-byte chunk size is accepted at FILE_START");
  check(isRefused(rxChunkRaw(kSender, 81, 0, kChunk), espnowanswer::kBadRequest) &&
            cancelCount(81, espnowanswer::kBadRequest) == 1 && eventCount(MSG_FILE_RECV_FAILED) == 1,
        "streaming: a chunk longer than chunkSize ends the transfer (bad_request cancel)");
  check(rxChunkRaw(kSender, 81, 1, 3).kind == V4VerdictKind::Refused, "streaming: nothing more is taken");
  runCmdExec();
  check(fileSlotsActiveCount() == 0 && partFiles() == 0 && fakeTakenBytes == 0 && liveAllocations == 0,
        "streaming: nothing was written; slot and buffers freed");
  // RAM path: a short middle chunk, and a long last chunk.
  resetWorld();
  const std::string bytes = pattern(1000, 41);
  rxStart(kSender, 82, "ram.bin", bytes.size());
  check(isRefused(rxChunkRaw(kSender, 82, 0, kChunk - 1), espnowanswer::kBadRequest) && fileSlotsActiveCount() == 0,
        "RAM path: a short chunk ends the transfer");
  rxStart(kSender, 83, "ram2.bin", bytes.size());
  for (uint16_t i = 0; i + 1 < chunksOf(bytes.size()); i++) rxChunk(kSender, 83, bytes, i);
  check(isRefused(rxChunkRaw(kSender, 83, chunksOf(bytes.size()) - 1, kChunk), espnowanswer::kBadRequest) &&
            fileSlotsActiveCount() == 0,
        "RAM path: a last chunk longer than the rest of totalSize ends the transfer");

  // A late chunk of a stored transfer draws no cancel.
  resetWorld();
  rxStart(kSender, 85, "done.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 85, bytes, i);
  rxEndOf(kSender, 85, bytes);
  rxChunk(kSender, 85, bytes, 1);
  check(resultCount(85, espnowanswer::kOk) == 1 && cancels.empty(),
        "a late chunk after the verdict: no FILE_CANCEL (the sender would read it as a failure)");
  // Cancels for unknown transfers: one per peer per 100 ms, whatever the id.
  resetWorld();
  rxChunk(kSender, 901, bytes, 0);
  rxChunk(kSender, 902, bytes, 0);
  rxChunk(kOther, 903, bytes, 0);
  check(cancels.size() == 2 && cancelCount(901) == 1 && cancelCount(903) == 1,
        "unknown transfers: one cancel per peer per 100 ms, new ids do not get around it");
  nowMs += 100;
  rxChunk(kSender, 904, bytes, 0);
  check(cancelCount(904) == 1, "unknown transfers: the next window gets its cancel");

  // The sender: a "rejected" cancel during the result wait (a late chunk) does
  // not end the send; the FILE_RESULT does.
  resetWorld();
  putSendFile(bytes);
  loopback = true;
  dropResults = 1;
  onAirFrame = [](const AirFrame& f) {
    if (f.type == ESPNOW_V4_TYPE_FILE_END && dropResults == 1) {
      uint32_t id = 0;
      std::memcpy(&id, f.payload.data(), 4);
      v4_send_file_cancel(kSender, id, FILE_CANCEL_REJECTED);
    }
  };
  check(sendFileToMacResult(kReceiver, String(kFile)).result == FileSendResult::Stored,
        "a rejected cancel after FILE_END is not the verdict: Stored from the FILE_RESULT");
}

int main(int argc, char** argv) {
  // With a case name, only that case runs (a case that leaves a slot behind on
  // a broken tree would otherwise blur the ones after it).
  const char* only = argc > 1 ? argv[1] : nullptr;
  const struct {
    const char* name;
    void (*run)();
  } cases[] = {
    {"ram-receive", testRamReceiveArrives},
    {"streaming-receive", testStreamingReceiveArrives},
    {"start-refusals", testStartRefusals},
    {"end-copy-result", testEndCopyGetsTheStoredResult},
    {"storage-admission", testStorageAdmissionAtAccept},
    {"storage-part-open", testStorageAdmissionAtPartOpen},
    {"storage-written-files", testStorageAdmissionCountsWrittenFiles},
    {"writer-busy", testWriterBusyDoesNotCancelHealthyReceive},
    {"teardown-claim", testTeardownKeepsTheSlotClaimed},
    {"busy-table", testBusyTableIsNotAMissingTransfer},
    {"contended-abort", testAbortUnderContentionDoesNotPinTheSlot},
    {"contended-drain", testDrainUnderContentionWritesEachBufferOnce},
    {"parked-teardown", testParkedTeardownUnderContention},
    {"send-with-session", testSendWithSessionArrives},
    {"send-without-session", testSendWithoutSessionIsRefused},
    {"session-lost", testSessionLostMidTransfer},
    {"send-failed-unknown", testSendFailedAndUnknown},
    {"send-bad-crc", testSendBadCrc},
    {"cancel-mid-pump", testCancelMidPump},
    {"name-too-long", testNameTooLong},
    {"start-refused-fast", testStartRefusedFast},
    {"callers", testCallersReportTheResult},
    {"bond-sync-quiet", testBondSyncFilesAreQuiet},
    {"cancel-answer", testCancelIsAnAnswer},
    {"ram-finish-after-ack", testRamFinishAfterAck},
    {"chunk-length-late", testChunkLengthAndLateChunks},
  };
  check(fileSlotsInit(), "slot table initialised");
  liveAllocations = 0;  // count transfer buffers only, not the table
  wireFs();
  unsigned ran = 0;
  for (const auto& c : cases) {
    if (only && std::strcmp(only, c.name) != 0) continue;
    c.run();
    ++ran;
  }
  check(ran > 0, "the named case exists");
  if (!only) resetWorld();
  std::printf("espnow_file_transfer harness: %u cases, %u checks, %u failures\n", ran, checks, failures);
  return failures ? 1 : 0;
}
