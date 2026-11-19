// Host harness for the ESP-NOW file transfer: receive slot table, FILE_*
// handlers, sender pump and the commands that report a send.
//
// test_espnow_file_transfer.py splices in production code at the INSERT tags:
// the whole implementation of System_ESPNow_Files.cpp, the FILE_START / DATA /
// END / CANCEL handlers and the streaming finalize job, sendFileToMac with its
// session gate, and espnowsendfile / espnowsend / imagesend / the OLED send job
// with the real CommandArgs. The wire structs, the receive context and the MAC
// text helpers are the real headers; the filesystem is fake_vfs.h.
//
// One thread plays every task. Where a task waits — for the filesystem lock, or
// in vTaskDelay — a hook lets the test play the other task. The slot-table
// mutex double then answers a take the way FreeRTOS would: a bounded take of a
// mutex whose holder is waiting further up the stack misses after its timeout;
// an unbounded one could never return and is counted as a deadlock.
//
// The radio is a loopback: a frame the sender hands to the smart send path is
// delivered to the receive handlers at once when the session is ACTIVE, and
// otherwise goes to a model of the pending-frame store (one frame per peer,
// newest wins). The session handshake completes only when the test says the
// command task is free, never inside the pump, because on the device the
// handshake is finished by a job on the task the pump is running on.
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

#ifdef HW1_FILE_SEND_BOOL_ONLY
// A tree from before the result type existed: the pump returns bool.
enum class FileSendResult : uint8_t { Sent = 0, Failed = 1, NoSession = 2 };
#endif
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
  volatile uint32_t fileSendMsgId = 0;
  volatile uint32_t fileSendAbortMsgId = 0;
  uint32_t fileTransfersSent = 0;
  uint32_t fileTransfersReceived = 0;
};
static EspNowState gEspNowStorage;
static EspNowState* gEspNow = &gEspNowStorage;

// The two devices. The sender's frames reach the receive handlers with the
// sender's address as source; the receiver's FILE_CANCEL reaches the sender's
// cancel handler with the receiver's address as source.
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

// A FILE_CANCEL as production's v4_send_file_cancel puts it on the air (rule
// F3): `msgId` is the payload's reqId (the transfer's FILE_START msgId),
// `reason` its status, `frameMsgId` the cancel frame's own header msgId.
struct Cancel {
  uint8_t to[6];
  uint32_t msgId;
  uint8_t reason;
  uint32_t frameMsgId;
};
static std::vector<Cancel> cancels;
static std::vector<uint32_t> acks;
static bool loopback = false;  // receiver replies are delivered to the sender
static void deliverCancelToSender(uint32_t frameMsgId, const uint8_t* payload, uint16_t len);
bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                           const uint8_t* payload, uint16_t len, uint8_t ttl);
uint32_t generateMessageId();
static unsigned borrowedCancelIds = 0;
// The FILE_CANCEL branch of the smart-send double (receiver -> sender).
static bool recordCancelFrame(const uint8_t* dst, uint32_t frameMsgId, const uint8_t* payload, uint16_t len) {
  Cancel c{};
  std::memcpy(c.to, dst, 6);
  c.frameMsgId = frameMsgId;
  check(len == sizeof(V4PayloadFileCancel) && len == espnowanswer::kHeaderLen &&
            espnowanswer::parseHeader(payload, len, &c.msgId, &c.reason),
        "FILE_CANCEL is the 5-byte answer {reqId, status}");
  if (frameMsgId == c.msgId) ++borrowedCancelIds;
  cancels.push_back(c);
  if (loopback) deliverCancelToSender(frameMsgId, payload, len);
  return true;
}

// ---- production: the FILE_CANCEL sender ----------------------------------------------------
// INSERT_CANCEL_TX
bool v4_send_ack(const uint8_t*, uint32_t ackFor) {
  acks.push_back(ackFor);
  return true;
}
static unsigned cancelCount(uint32_t msgId) {
  unsigned n = 0;
  for (const auto& c : cancels) if (c.msgId == msgId) ++n;
  return n;
}
static unsigned cancelCount(uint32_t msgId, uint8_t reason) {
  unsigned n = 0;
  for (const auto& c : cancels) if (c.msgId == msgId && c.reason == reason) ++n;
  return n;
}

// ---- production: receive handlers ---------------------------------------------------------
// INSERT_RX

// One received frame, as the dispatcher hands it to a handler.
static void rxFrame(uint8_t type, uint8_t* src, uint32_t msgId, const uint8_t* payload, uint16_t len) {
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
  switch (type) {
    case ESPNOW_V4_TYPE_FILE_START:  v4h_file_start(ctx); break;
    case ESPNOW_V4_TYPE_FILE_DATA:   v4h_file_data(ctx); break;
    case ESPNOW_V4_TYPE_FILE_END:    v4h_file_end(ctx); break;
    case ESPNOW_V4_TYPE_FILE_CANCEL: v4h_file_cancel(ctx); break;
    default: check(false, "rxFrame: unexpected frame type");
  }
}
static void deliverCancelToSender(uint32_t frameMsgId, const uint8_t* payload, uint16_t len) {
  rxFrame(ESPNOW_V4_TYPE_FILE_CANCEL, kReceiver, frameMsgId, payload, len);
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
static void putOnAir(const AirFrame& f) {
  air.push_back(f);
  if (onAirFrame) onAirFrame(f);
  if (loopback) rxFrame(f.type, kSender, f.msgId, f.payload.data(), static_cast<uint16_t>(f.payload.size()));
}
// v4_send_payload_smart for one frame to the receiver: session-wrapped at once
// when the session is ACTIVE, else parked behind the frames already waiting
// (refused when 4 wait) with a handshake kick when none is in flight.
bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t, uint32_t msgId,
                           const uint8_t* payload, uint16_t len, uint8_t) {
  if (type == ESPNOW_V4_TYPE_FILE_CANCEL) return recordCancelFrame(dst, msgId, payload, len);
  if (std::memcmp(dst, kReceiver, 6) != 0) check(false, "smart send: unexpected destination");
  AirFrame f{type, msgId, std::vector<uint8_t>(payload, payload + len)};
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
uint16_t v4_single_frame_cap(const uint8_t*) { return ESPNOW_V4_MAX_PLAINTEXT; }

static AuthContext callerAuth;
const AuthContext& currentAuthContext() { return callerAuth; }
static bool readAllowed = true;
bool canRead(const String&, const AuthContext&) { return readAllowed; }
static uint32_t nextMessageId = 500;
uint32_t generateMessageId() { return ++nextMessageId; }

// ---- production: sender pump -----------------------------------------------------------------
// INSERT_TX

#ifdef HW1_FILE_SEND_BOOL_ONLY
static FileSendResult sendFileToMacResult(const uint8_t* mac, const String& path) {
  return sendFileToMac(mac, path) ? FileSendResult::Sent : FileSendResult::Failed;
}
#endif

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
                    uint32_t resolvedAtMs; };
static unsigned statusReads = 0, deliveryStamps = 0;
static uint8_t statusState = SEND_STATUS_PENDING;  // what the status table says when re-read
static uint32_t stampMsgId = 0;
static uint8_t stampState = 0xFF, stampMac[6] = {};
bool sendStatusGet(uint32_t msgId, SendStatus* out) {
  ++statusReads;
  if (out) *out = SendStatus{true, statusState, {}, msgId, 0, 0};
  return true;
}
void espnowUpdateSentDeliveryState(const uint8_t* mac, uint32_t msgId, uint8_t state) {
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
    air.push_back({type, msgId, std::vector<uint8_t>(payload, payload + len)});
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
static constexpr uint16_t kChunk = ESPNOW_V4_MAX_PLAINTEXT - 2;  // what the sender uses
static uint16_t chunksOf(size_t size) { return static_cast<uint16_t>((size + kChunk - 1) / kChunk); }

static void rxStart(uint8_t* src, uint32_t msgId, const char* name, size_t size) {
  V4PayloadFileStart s{};
  s.fileSize = static_cast<uint32_t>(size);
  s.chunkCount = chunksOf(size);
  s.chunkSize = kChunk;
  std::strncpy(s.filename, name, sizeof(s.filename) - 1);
  rxFrame(ESPNOW_V4_TYPE_FILE_START, src, msgId, reinterpret_cast<const uint8_t*>(&s), sizeof(s));
}
static void rxChunk(uint8_t* src, uint32_t msgId, const std::string& bytes, uint16_t index) {
  V4PayloadFileData d{};
  d.chunkIndex = index;
  const size_t offset = static_cast<size_t>(index) * kChunk;
  const size_t n = std::min<size_t>(kChunk, bytes.size() - offset);
  std::memcpy(d.data, bytes.data() + offset, n);
  rxFrame(ESPNOW_V4_TYPE_FILE_DATA, src, msgId, reinterpret_cast<const uint8_t*>(&d), static_cast<uint16_t>(2 + n));
}
// FILE_END's status: 0 = the sender sent the whole file (0 is ok in every
// status byte, rule F3); anything else (kCancelled) = it stopped.
static void rxEnd(uint8_t* src, uint32_t msgId, uint32_t crc, uint8_t status = espnowanswer::kOk) {
  V4PayloadFileEnd e{};
  e.crc32 = crc;
  e.status = status;
  rxFrame(ESPNOW_V4_TYPE_FILE_END, src, msgId, reinterpret_cast<const uint8_t*>(&e), sizeof(e));
}
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
static uint8_t slotState(uint32_t msgId) {
  for (uint8_t i = 0; i < fileSlotsSlotCount(); i++) {
    FileTransferSlotInfo info{};
    if (fileSlotsSnapshot(i, &info) && info.msgId == msgId) return info.state;
  }
  return FILE_SLOT_FREE;
}

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
  cancels.clear(); acks.clear(); events.clear(); broadcasts.clear(); borrowedCancelIds = 0;
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
  rxStart(kSender, 11, "note.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 11, bytes, i);
  rxEnd(kSender, 11, crcOf(bytes));
  check(fake_vfs::fs().text(inboxPath(kSender, "note.txt")) == bytes, "RAM path: the file arrives byte for byte");
  check(acks.size() == 1 && acks[0] == 11, "RAM path: FILE_END is acknowledged once");
  check(cancels.empty(), "RAM path: a healthy transfer gets no FILE_CANCEL");
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
  rxEnd(kSender, 12, crcOf(bytes));
  check(slotState(12) == FILE_SLOT_COMPLETING, "streaming: FILE_END hands the slot to the finalize job");
  runCmdExec();
  check(fake_vfs::fs().text(inboxPath(kSender, "big.bin")) == bytes, "streaming: the file arrives byte for byte");
  check(acks.size() == 1 && acks[0] == 12, "streaming: the finalize job acknowledges once");
  check(cancels.empty(), "streaming: a healthy transfer gets no FILE_CANCEL");
  check(fileSlotsActiveCount() == 0, "streaming: the finalize job frees the slot");
  check(partFiles() == 0, "streaming: no staging file is left");
  check(fake_vfs::fs().liveHandles() == 0, "streaming: the staging handle is closed");
  check(fsCallsUnderSlotLock == 0,
        "release does no filesystem work (close, remove) with the slot lock held");
  check(fsLockWaitsUnderSlotLock == 0, "release does not wait for the filesystem lock with the slot lock held");
  check(deadlocks == 0 && boundedMisses == 0, "streaming: no lock wait went wrong");
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
  rxEnd(kSender, 21, 0, espnowanswer::kCancelled);  // the sender aborted A: teardown is queued
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
  rxEnd(kOther, 22, crcOf(b));
  check(fake_vfs::fs().text(inboxPath(kOther, "b.txt")) == b, "writer busy: B arrives byte for byte");
  check(cancelCount(22) == 0, "writer busy: B never gets a FILE_CANCEL");
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
  rxEnd(kSender, 31, 0, espnowanswer::kCancelled);
  uint8_t stateDuringFsWork = FILE_SLOT_FREE;
  uint8_t slotsInUse = 0;
  onFsLockWait = [&] {
    stateDuringFsWork = slotState(31);
    rxStart(kSender, 32, "a.bin", a.size());   // a retry of the same file from the same peer
    slotsInUse = fileSlotsActiveCount();
  };
  runCmdExec();
  check(stateDuringFsWork == FILE_SLOT_COMPLETING, "teardown: the slot stays claimed during the filesystem work");
  check(slotsInUse == 2, "teardown: a retry that arrives meanwhile gets a different slot");
  check(cancelCount(32) == 0, "teardown: the retry is accepted");
  check(slotState(31) == FILE_SLOT_FREE && slotState(32) == FILE_SLOT_RECEIVING,
        "teardown: the old slot is freed, the retry is receiving");
  rxChunksWithWriter(kSender, 32, a, 0, chunksOf(a.size()));
  rxEnd(kSender, 32, crcOf(a));
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
  rxEnd(kSender, 41, crcOf(bytes));
  check(cancels.size() == 1 && cancelCount(41, FILE_CANCEL_INCOMPLETE) == 1,
        "the dropped chunk is reported once, at FILE_END, as INCOMPLETE");
  check(eventCount(MSG_FILE_RECV_FAILED) == 1 && acks.empty(), "the incomplete transfer is recorded as failed");
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
  rxEnd(kSender, 44, crcOf(bytes));
  check(fake_vfs::fs().text(inboxPath(kSender, "end.txt")) == bytes && acks.size() == 1 && cancels.empty(),
        "FILE_END survives one busy lookup: the file is stored and acknowledged");

  // FILE_END with the table busy for both tries: no verdict; the sweep reports it.
  resetWorld();
  rxStart(kSender, 45, "late.txt", bytes.size());
  for (uint16_t i = 0; i < chunksOf(bytes.size()); i++) rxChunk(kSender, 45, bytes, i);
  slotLockHeldElsewhereMs = 120;
  rxEnd(kSender, 45, crcOf(bytes));
  check(cancels.empty() && acks.empty(), "FILE_END dropped on a busy table sends neither cancel nor ack");
  check(slotState(45) == FILE_SLOT_RECEIVING, "FILE_END dropped on a busy table leaves the slot for the sweep");
  slotLockHeldElsewhereMs = 0;
  FileSlotExpiry expired[4];
  nowMs += kFileSlotTimeoutMs + 1;
  check(fileSlotsTimeoutSweep(nowMs, expired, 4) == 1 && expired[0].msgId == 45,
        "the sweep reports the transfer whose FILE_END was dropped");
  check(fileSlotsActiveCount() == 0, "the sweep releases it");
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
  rxEnd(kSender, 61, crcOf(a));
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
  rxEnd(kSender, 71, crcOf(a));
  check(cancelCount(71, FILE_CANCEL_WRITE_FAILED) == 1, "parked teardown: the sender is told the write failed");
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

static void testSendWithSessionArrives() {
  resetWorld();
  const std::string bytes = pattern(1000, 20);
  putSendFile(bytes);
  loopback = true;
  receiverKeepsUp();
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::Sent, "send with a session: reported as sent");
  runCmdExec();
  check(air.size() == 2u + chunksOf(bytes.size()), "send with a session: START, every chunk and END left the sender");
  check(fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes,
        "send with a session: the receiver has the file byte for byte");
  check(sessionOpenKicks == 0 && keyExKicks == 0 && !pendingInUse(), "send with a session: no handshake, nothing parked");
  check(cancels.empty() && !gEspNow->fileSendInProgress, "send with a session: no cancel, the send flag is cleared");

  // A file large enough to stream on the receiver.
  resetWorld();
  const std::string big = pattern(kStreamSize, 21);
  putSendFile(big);
  loopback = true;
  receiverKeepsUp();
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::Sent, "streaming send: reported as sent");
  runCmdExec();
  check(fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == big,
        "streaming send: the receiver has the file byte for byte");
  check(cancels.empty() && fileSlotsActiveCount() == 0, "streaming send: no cancel, the receive slot is freed");
}

static void testSendWithoutSessionIsRefused() {
  resetWorld();
  const std::string bytes = pattern(1000, 22);
  putSendFile(bytes);
  loopback = true;
  receiverKeepsUp();
  receiverSession.state = SESSION_FREE;
  const uint32_t before = nowMs;
  const FileSendResult result = sendFileToMacResult(kReceiver, String(kFile));
  const bool reportedSent = (result == FileSendResult::Sent);
  // The pump has returned: the command task is free and finishes the handshake.
  commandTaskCompletesHandshake();
  runCmdExec();
  const bool arrived = (fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes);
  check(!reportedSent || arrived, "a send started without a session must not report success for a file the receiver never got");
  check(result == FileSendResult::NoSession, "a send without an ACTIVE session is refused as NoSession");
  check(air.empty() && pendingParked == 0, "a refused send puts no frame on air and none in the pending store");
  check(sessionOpenKicks == 1, "a refused send starts the session handshake once");
  check(nowMs == before, "a refused send does not wait on the command task");
  check(!gEspNow->fileSendInProgress && fake_vfs::fs().liveHandles() == 0, "a refused send leaves no send flag and no open file");

  // The retry the reply asks for.
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::Sent, "the retry after the handshake is sent");
  runCmdExec();
  check(fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes, "the retry arrives byte for byte");

  // A handshake or a rekey that is already in flight is left alone.
  resetWorld();
  putSendFile(bytes);
  receiverSession.state = SESSION_ESTABLISHING;
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::NoSession, "handshake in flight: refused");
  check(sessionOpenKicks == 0 && receiverSession.state == SESSION_ESTABLISHING, "handshake in flight: not restarted");
  receiverSession.state = SESSION_REKEYING;
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::NoSession, "rekey in flight: refused");
  check(sessionOpenKicks == 0 && receiverSession.state == SESSION_REKEYING, "rekey in flight: the working session is not reset");
  check(air.empty() && !pendingInUse(), "in-flight handshake: no frame on air or parked");

  // No session slot at all, and a closed one: each is kicked once.
  receiverHasSessionSlot = false;
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::NoSession && sessionOpenKicks == 1,
        "no session slot: refused, handshake started");
  receiverSession.state = SESSION_CLOSED;
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::NoSession && sessionOpenKicks == 2,
        "closed session: refused, handshake started");

  // No identity yet: the key exchange is started once.
  resetWorld();
  putSendFile(bytes);
  receiverHasIdentity = false;
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::NoSession && keyExKicks == 1 && sessionOpenKicks == 0,
        "no peer identity: refused, key exchange started");
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::NoSession && keyExKicks == 1,
        "no peer identity: a key exchange in flight is not started again");
  check(air.empty() && !pendingInUse(), "no peer identity: no frame on air or parked");

  // A handshake that cannot be started is a failure, not "try again": nothing
  // is being opened (no shared mesh key, no free session slot, radio failure).
  resetWorld();
  putSendFile(bytes);
  receiverHasSessionSlot = false;
  kicksFail = true;
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::Failed && sessionOpenKicks == 1,
        "SESSION_OPEN cannot be started: Failed, not NoSession");
  receiverHasIdentity = false;
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::Failed && keyExKicks == 1,
        "key exchange cannot be started: Failed, not NoSession");
  check(air.empty() && !pendingInUse(), "handshake cannot start: no frame on air or parked");

  // The bool form, which the bond document senders and the FS_GET server use.
  resetWorld();
  putSendFile(bytes);
  receiverSession.state = SESSION_FREE;
  const bool boolResult = sendFileToMac(kReceiver, String(kFile));
  commandTaskCompletesHandshake();
  check(!boolResult, "sendFileToMac (bool) is false for a send without a session");

  // A file problem is reported as such, and starts no handshake.
  resetWorld();
  receiverSession.state = SESSION_FREE;
  check(sendFileToMacResult(kReceiver, String("/data/missing.bin")) == FileSendResult::Failed && sessionOpenKicks == 0,
        "a missing file is a failure, not a session refusal, and starts no handshake");
  putSendFile(bytes);
  readAllowed = false;
  check(sendFileToMacResult(kReceiver, String(kFile)) == FileSendResult::Failed && sessionOpenKicks == 0,
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
  const FileSendResult result = sendFileToMacResult(kReceiver, String(kFile));
  commandTaskCompletesHandshake();
  runCmdExec();
  const bool arrived = (fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes);
  check(result != FileSendResult::Sent || arrived, "a session lost mid-transfer must not be reported as sent");
  check(result == FileSendResult::Failed, "a session lost mid-transfer is a failed send");
  check(dataFrames == 5 && pendingParked == 0, "after the session is lost no further chunk is parked");
  check(sessionOpenKicks == 0, "the pump does not restart the session itself");
  check(!gEspNow->fileSendInProgress && fake_vfs::fs().liveHandles() == 0, "the aborted send closes its file and clears its flag");
  // The receiver's slot is left to its timeout sweep.
  FileSlotExpiry expired[4];
  nowMs += kFileSlotTimeoutMs + 1;
  check(fileSlotsTimeoutSweep(nowMs, expired, 4) == 1, "the receiver's sweep reclaims the abandoned transfer");
}

static void testCallersSayThereIsNoSession() {
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
  check(reply.rfind("Error:", 0) == 0 && reply.find("sent") == std::string::npos && reply.find("success") == std::string::npos,
        "espnowsendfile without a session: an error the web card cannot read as success");
  check(gEspNow->fileTransfersSent == 0 && eventCount(MSG_FILE_SEND_SUCCESS) == 0,
        "espnowsendfile without a session: nothing counted or recorded as sent");
  reply = cmd_espnow_sendfile(String("peerB \"/data/report.bin\""));
  runCmdExec();
  check(reply == "File sent successfully via v3: report.bin (1000 bytes)", "espnowsendfile with a session: success reply unchanged");
  check(gEspNow->fileTransfersSent == 1 && fake_vfs::fs().text(inboxPath(kSender, "report.bin")) == bytes,
        "espnowsendfile with a session: counted, and the file arrived");

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
  check(hints.empty(), "imagesend without a session: no 'lands in the inbox' hint");
  reply = cmd_imagesend(String("peerB \"/data/report.bin\""));
  runCmdExec();
  check(reply == "Sent /data/report.bin to peerB" && hints.size() == 1, "imagesend with a session: success reply unchanged");

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
  check(toasts.size() == 1 && toasts[0] == "No session yet. Try again.", "OLED send without a session: says so, not 'File sent'");
  runEspNowSendFile(job());
  runCmdExec();
  check(toasts.size() == 2 && toasts[1] == "File sent", "OLED send with a session: 'File sent' unchanged");

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
  // The chunked send resolved the msgId (delivered) before the sent record
  // existed: the re-read stamps it onto the record, after the record is stored.
  statusState = SEND_STATUS_DELIVERED;
  reply = cmd_espnow_send(String(("peerB " + longText).c_str()));
  check(reply.rfind("Encrypted message sent via SESSION_FRAME", 0) == 0 && historyStored == 2 && statusReads == 2,
        "espnowsend (multi-frame, delivered during the send): sent");
  check(deliveryStamps == 1 && historyStoredAtStamp == 2 && stampMsgId == lastRegisteredMsgId && stampMsgId != 0 &&
            stampState == SEND_STATUS_DELIVERED && std::memcmp(stampMac, kReceiver, 6) == 0,
        "espnowsend: a state resolved during the send is stamped on the stored record (peer, msgId, delivered)");

  // One frame still rides the pending store across the handshake; the reply
  // says it waits for the session, not that it was sent (bench 2026-11-18).
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
  // With the session up the same send is reported sent.
  reply = cmd_espnow_send(String("peerB hello"));
  check(reply.rfind("Encrypted message sent via SESSION_FRAME", 0) == 0 && air.size() == 2,
        "espnowsend with a session: reported sent");

  // Bench 2026-11-18: two (and up to four) espnowsend in a row before the
  // session opened; the second used to evict the first while both replied
  // "sent". Each is kept, each says queued, and they go out in order.
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
  // A fifth is refused with the reason and one next step; nothing waiting is dropped.
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
  // json: a queued send carries "queued":true.
  resetWorld();
  receiverSession.state = SESSION_FREE;
  reply = cmd_espnow_send(String("json peerB hi"));
  check(reply.find("\"ok\":true") != std::string::npos && reply.find(",\"queued\":true}") != std::string::npos,
        "json: a queued send says queued");
}

// Bench 2026-11-18: the bond's own sync files (the partner's settings and
// automation list on every resync, its manifest) raised a file_rx event each
// time; eventpush flooded the mesh master's event list with them. Only these
// names, only from the bond partner, only with bond mode on.
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

// Rule F3: FILE_CANCEL is an answer. The receiver puts the transfer's msgId
// in the payload's reqId and its reason in status, under a fresh header msgId
// of its own; the sender stops the transfer the reqId names and reports it
// once, whatever header msgIds the repeats carry.
static void testCancelIsAnAnswer() {
  resetWorld();
  loopback = true;
  gEspNow->fileSendInProgress = true;
  std::memcpy(gEspNow->fileSendPeer, kReceiver, 6);
  gEspNow->fileSendMsgId = 777;
  v4_send_file_cancel(kSender, 777, FILE_CANCEL_INCOMPLETE);
  check(cancels.size() == 1 && cancels[0].msgId == 777 && cancels[0].reason == espnowanswer::kFileIncomplete,
        "cancel answer: reqId = the transfer's msgId, status incomplete (16)");
  check(borrowedCancelIds == 0 && cancels[0].frameMsgId != 777,
        "cancel answer: its header msgId is the canceller's own, not the transfer's");
  check(gEspNow->fileSendAbortMsgId == 777, "cancel answer: the sender stops the transfer the reqId names");
  check(eventCount(MSG_FILE_SEND_FAILED) == 1, "cancel answer: the failed send is recorded");
  // A repeat (another fresh header msgId) is not recorded again.
  v4_send_file_cancel(kSender, 777, FILE_CANCEL_INCOMPLETE);
  check(cancels.size() == 2 && cancels[1].frameMsgId != cancels[0].frameMsgId,
        "cancel answer: each repeat has a header msgId of its own");
  check(eventCount(MSG_FILE_SEND_FAILED) == 1, "cancel answer: a repeat is not recorded twice");
  // A cancel for another transfer leaves this one running.
  gEspNow->fileSendAbortMsgId = 0;
  nowMs += 6000;
  v4_send_file_cancel(kSender, 778, FILE_CANCEL_CRC_MISMATCH);
  check(gEspNow->fileSendAbortMsgId == 0, "cancel answer: another transfer's cancel stops nothing");
  gEspNow->fileSendInProgress = false;
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
    {"writer-busy", testWriterBusyDoesNotCancelHealthyReceive},
    {"teardown-claim", testTeardownKeepsTheSlotClaimed},
    {"busy-table", testBusyTableIsNotAMissingTransfer},
    {"contended-abort", testAbortUnderContentionDoesNotPinTheSlot},
    {"contended-drain", testDrainUnderContentionWritesEachBufferOnce},
    {"parked-teardown", testParkedTeardownUnderContention},
    {"send-with-session", testSendWithSessionArrives},
    {"send-without-session", testSendWithoutSessionIsRefused},
    {"session-lost", testSessionLostMidTransfer},
    {"callers", testCallersSayThereIsNoSession},
    {"bond-sync-quiet", testBondSyncFilesAreQuiet},
    {"cancel-answer", testCancelIsAnAnswer},
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
