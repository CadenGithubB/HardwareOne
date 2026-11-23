// Host harness for rule F3 of the 1.0 ESP-NOW format: every answer starts
// {u32 reqId, u8 status} and goes under the answerer's own fresh msgId.
// test_espnow_answers.py inserts, unchanged from System_ESPNow.cpp, the CMD_RESP
// senders (v4_send_command_response, v4_send_cmd_resp_async), the STREAM line
// sender (v4_send_stream_line) and the two receivers (v4h_cmd_resp,
// v4h_stream). The radio is a double that records each frame; a recorded frame
// is then handed to the receiver as the dispatcher would, with the frame's own
// header msgId, so a receiver that still read the header for the correlation
// sees the wrong number.
//
// The command path is inserted unchanged as well: v4h_cmd (the one-slot busy
// answer: the ACK only, since 1.0 step 6), v4CmdResultCallback and
// v4_send_cmd_result (every remote command's result, sent with an ACK wait) and
// v4_handle_cmd from its authentication decision on. The stream-session
// table, the command queue and the executor are doubles; the executor's
// callback is called by the cases as cmd_exec would. The requester's result
// queue is the real System_ESPNow_RemoteCmdCore.h (16 entries, 24 KB ring):
// a burst fills it and the next result is refused busy.
#include <Arduino.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <atomic>

#include "System_ESPNow_RxCtx.h"  // V4RxCtx over the real System_ESPNow_Wire.h
#include "System_DeviceName.h"
#include "System_ESPNow_RemoteCmdCore.h"

static unsigned checks = 0, failures = 0;
static void check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", what);
  }
}

// ---- doubles -----------------------------------------------------------------------------
#define DEBUG_ESPNOWF(...) do {} while (0)
static std::vector<std::string> broadcasts;
#define BROADCAST_PRINTF(...)                                   \
  do {                                                          \
    char line_[256];                                            \
    std::snprintf(line_, sizeof(line_), __VA_ARGS__);           \
    broadcasts.push_back(line_);                                \
  } while (0)
#define V4_REASM_MAX_MSG 6400

enum class AllocPref { PreferPsram, PreferInternal };
static bool allocFails = false;  // the result callback's out-of-memory fallback
static void* ps_alloc(size_t n, AllocPref, const char*) { return allocFails ? nullptr : std::malloc(n); }
static void macToDisplay(const uint8_t* mac, char* out, size_t len) {
  std::snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// This device's own message numbers. The requests in the cases use small
// numbers, so a borrowed one is plain to see.
static uint32_t nextOwnMsgId = 0x70000000u;
uint32_t generateMessageId() { return ++nextOwnMsgId; }

struct AirFrame {
  uint8_t dst[6];
  uint8_t type;
  uint16_t flags;
  uint32_t msgId;
  std::vector<uint8_t> payload;
  bool sync;  // sent through the blocking smart path
};
static std::vector<AirFrame> air;
static uint16_t frameCap = ESPNOW_V4_MAX_PLAINTEXT;  // 202 direct, 146 routed
uint16_t v4_single_frame_cap(const uint8_t*) { return frameCap; }
namespace espnowtx {
bool sendAead(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId, const uint8_t* payload,
              uint16_t len, uint8_t) {
  AirFrame f{};
  std::memcpy(f.dst, dst, 6);
  f.type = type;
  f.flags = flags;
  f.msgId = msgId;
  f.payload.assign(payload, payload + len);
  air.push_back(f);
  return true;
}
}  // namespace espnowtx
// The session the acknowledged CMD_RESP sender looks up (System_ESPNow_Sessions.h).
struct PeerIdentity { uint8_t meshId; };
static PeerIdentity gPeerIdentity{0};
static const PeerIdentity* peerIdentityFindByMac(const uint8_t*) { return &gPeerIdentity; }
enum { SESSION_FREE = 0, SESSION_ACTIVE = 3 };
struct SessionState { int state; };
static SessionState gSession{SESSION_ACTIVE};
static bool sessionUp = true;
static SessionState* sessionFindByPeer(const uint8_t*, uint8_t) { return sessionUp ? &gSession : nullptr; }
enum class V4SendOutcome : uint8_t { Delivered = 0, Refused = 1, NoAnswer = 2, NotSent = 3 };
struct V4SendResult { V4SendOutcome outcome; uint8_t status; };
// v4_send_encrypted_chunked with ackSingle: records the frame and how it went.
static std::vector<bool> ackedSends;
static V4SendResult chunkedResult{V4SendOutcome::Delivered, 0};
V4SendResult v4_send_encrypted_chunked(const uint8_t dst[6], uint8_t type, uint16_t flags, uint32_t msgId,
                                       const uint8_t* payload, uint16_t len, uint8_t ttl, bool ackSingle);
bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                           const uint8_t* payload, uint16_t len, uint8_t ttl) {
  const bool ok = espnowtx::sendAead(dst, type, flags, msgId, payload, len, ttl);
  air.back().sync = true;
  return ok;
}
namespace espnowtx {
bool sendAeadSync(const uint8_t mac[6], uint8_t type, uint16_t flags, uint32_t msgId, const uint8_t* payload,
                  uint16_t payloadLen, uint8_t ttl, uint32_t) {
  const bool ok = sendAead(mac, type, flags, msgId, payload, payloadLen, ttl);
  air.back().sync = true;
  return ok;
}
}  // namespace espnowtx
V4SendResult v4_send_encrypted_chunked(const uint8_t dst[6], uint8_t type, uint16_t flags, uint32_t msgId,
                                       const uint8_t* payload, uint16_t len, uint8_t ttl, bool ackSingle) {
  espnowtx::sendAead(dst, type, flags, msgId, payload, len, ttl);
  air.back().sync = true;
  ackedSends.push_back(ackSingle);
  return chunkedResult;
}

// ---- the command path's doubles ---------------------------------------------------------
#define DEBUG_ESPNOW_ROUTERF(...) do {} while (0)
#define MAC_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_ARGS(mac) (mac)[0], (mac)[1], (mac)[2], (mac)[3], (mac)[4], (mac)[5]
#define MSG_ROUTE_WEB 0x02
#define MSG_ROUTE_FILE 0x04
constexpr char kCommandReplyOutOfMemory[] = "Error: Out of memory. Free memory and retry the command.";
static String redactOutputForLog(const String& s) { return s; }
static String redactCmdForAudit(const String& s) { return s; }
static unsigned long millis() { return 1000; }
enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_ESPNOW };
enum CommandSource { SOURCE_SERIAL, SOURCE_ESPNOW };
enum { SYSEVT_REMOTE_CMD_RX = 1 };
static void systemEventPost(int, const char*, const char*) {}
struct AuthContext {
  CommandSource transport;
  String path, ip, user, sid;
  void* opaque;
};
struct CommandContext {
  CommandOrigin origin = ORIGIN_WEB;
  AuthContext auth;
  uint32_t id = 0;
  uint32_t timestampMs = 0;
  uint32_t outputMask = 0;
  bool validateOnly = false;
  void* replyHandle = nullptr;
  void* httpReq = nullptr;
};
struct Command {
  String line;
  CommandContext ctx;
};
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);
static size_t inputMax = 64;
static bool commandInputLengthAccepted(size_t length) { return length <= inputMax; }
static bool queueTakes = true;
static ExecAsyncCallback queuedCallback = nullptr;
static void* queuedUserData = nullptr;
static uint32_t queuedId = 0;
static bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData) {
  if (!queueTakes) return false;
  queuedCallback = callback;
  queuedUserData = userData;
  queuedId = cmd.ctx.id;
  return true;
}
struct StreamSession {
  uint32_t cmdMsgId;
};
static bool sessionFree = true;
static StreamSession theSession{};
static std::vector<uint32_t> sessionsEnded;
static StreamSession* createStreamSession(const uint8_t*, uint32_t cmdMsgId) {
  if (!sessionFree) return nullptr;
  theSession.cmdMsgId = cmdMsgId;
  return &theSession;
}
static void destroyStreamSession(uint32_t cmdMsgId) { sessionsEnded.push_back(cmdMsgId); }

// ---- the topology receivers' doubles (System_ESPNow.h, one stream slot) -----------------
#define DEBUG_ESPNOW_TOPOF(...) do {} while (0)
static unsigned warnings = 0;
#define WARN_ESPNOWF(...)                                       \
  do {                                                          \
    ++warnings;                                                 \
    char warn_[200];                                            \
    std::snprintf(warn_, sizeof(warn_), __VA_ARGS__);           \
  } while (0)
#define MESH_PEER_MAX 16
struct TopologyPeerRecord {
  uint8_t mac[6];
  char name[32];
  int8_t rssi;
  bool present;
};
struct TopologyStream {
  uint32_t reqId;
  uint8_t senderMac[6];
  char senderName[32];
  uint16_t totalPeers;
  uint16_t receivedPeers;
  uint16_t receivedPeerMask;
  bool active;
  bool started;
  bool complete;
  TopologyPeerRecord peers[MESH_PEER_MAX];
};
struct TopoStreamsGuard {
  bool held = true;
  explicit TopoStreamsGuard(const char*) {}
};
static void* gTopoStreamsMutex = nullptr;
static uint32_t gTopoRequestId = 0, gTopoRequestTimeout = 0, gTopoLastResponseTime = 0;
struct MeshPeerMeta {
  bool isActive;
  uint8_t mac[6];
  char name[32];
};
static MeshPeerMeta* gMeshPeerMeta = nullptr;
static int gMeshPeerSlots = 0;
static TopologyStream topoSlot;
static bool topoSlotUsed = false;
static unsigned topoFinalized = 0;
static bool topologySenderExpectedLocked(const uint8_t*) { return true; }
static TopologyStream* findTopoStreamLocked(const uint8_t* mac, uint32_t reqId) {
  return topoSlotUsed && topoSlot.reqId == reqId && std::memcmp(topoSlot.senderMac, mac, 6) == 0 ? &topoSlot : nullptr;
}
static TopologyStream* findOrCreateTopoStreamLocked(const uint8_t* mac, uint32_t reqId) {
  if (TopologyStream* s = findTopoStreamLocked(mac, reqId)) return s;
  topoSlot = TopologyStream{};
  topoSlot.reqId = reqId;
  std::memcpy(topoSlot.senderMac, mac, 6);
  topoSlot.active = true;
  topoSlotUsed = true;
  return &topoSlot;
}
static void addTopoDeviceName(const uint8_t*, const char*) {}
static bool getTopoDeviceName(const uint8_t*, char*, size_t) { return false; }
static void finalizeTopologyStreamLocked(TopologyStream* s) {
  s->active = false;
  s->complete = true;
  ++topoFinalized;
}

// The parts of EspNowState the two receivers touch (System_ESPNow.h).
struct EspNowState {
  struct Device {
    uint8_t mac[6];
    String name;
  };
  Device devices[1];
  int deviceCount = 0;
  bool deferredCmdPending = false;
  uint8_t deferredCmdSrcMac[6] = {};
  char deferredCmdDeviceName[32] = {};
  char deferredCmdPayload[256] = {};
  uint16_t deferredCmdPayloadLen = 0;
  uint32_t deferredCmdMsgId = 0;
  bool deferredCmdWasEncrypted = false;
  struct StreamQueueEntry {
    uint8_t srcMac[6];
    char deviceName[32];
    char content[256];
    uint32_t cmdMsgId;
    bool used;
  };
  StreamQueueEntry* streamQueue = nullptr;
  int streamQueueMask = 0;
  volatile int streamQueueHead = 0;
  volatile int streamQueueTail = 0;
  uint32_t streamDroppedCount = 0;
  uint32_t streamReceivedCount = 0;
};
static EspNowState gState;
static EspNowState* gEspNow = &gState;
static EspNowState::StreamQueueEntry gQueue[16];

// The requester's store (System_ESPNow.cpp, RemoteCmdStore): the real result
// queue over a 24 KB ring; the lab hold; the STREAM deadline hook.
struct RemoteCmdStore {
  espnowcmd::RequesterTable requesters;
  espnowcmd::ResultQueue results;
  uint8_t ring[espnowcmd::kResultRingBytes];
};
static RemoteCmdStore gStore;
static RemoteCmdStore* gRemoteCmd = &gStore;
static std::atomic<uint32_t> gRemoteCmdHoldMs{0};
static std::vector<uint32_t> streamExtended;  // reqIds whose deadline a STREAM line moved
static void remoteCmdOnStream(const uint8_t*, uint32_t reqId) { streamExtended.push_back(reqId); }
// The newest queued result (what the old single slot held).
static const espnowcmd::ResultEntry* lastQueued() {
  const espnowcmd::ResultQueue& q = gStore.results;
  return q.count ? &q.e[(q.head + q.count - 1) % espnowcmd::kResults] : nullptr;
}
static std::string lastQueuedText() {
  const espnowcmd::ResultEntry* r = lastQueued();
  return r ? std::string(espnowcmd::textOf(gStore.results, *r)) : std::string();
}

// ---- production -------------------------------------------------------------------------
// INSERT_PRODUCTION

// ---- cases --------------------------------------------------------------------------------
static uint8_t kRequester[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0A};
static uint8_t kAnswerer[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0B};

static void resetWorld() {
  air.clear();
  broadcasts.clear();
  gState = EspNowState{};
  std::memset(&gStore.requesters, 0, sizeof(gStore.requesters));
  espnowcmd::queueInit(gStore.results, gStore.ring, sizeof(gStore.ring));
  gRemoteCmdHoldMs = 0;
  streamExtended.clear();
  ackedSends.clear();
  chunkedResult = V4SendResult{V4SendOutcome::Delivered, 0};
  sessionUp = true;
  warnings = 0;
  std::memset(gQueue, 0, sizeof(gQueue));
  gState.streamQueue = gQueue;
  gState.streamQueueMask = 15;
  frameCap = ESPNOW_V4_MAX_PLAINTEXT;
  allocFails = false;
  inputMax = 64;
  queueTakes = true;
  queuedCallback = nullptr;
  queuedUserData = nullptr;
  queuedId = 0;
  sessionFree = true;
  sessionsEnded.clear();
}

// Hands a recorded frame to a receive handler as the dispatcher would: the
// frame's own header msgId, the answerer as source.
static V4Verdict gLastReceiveVerdict{};
static void receive(const AirFrame& f) {
  esp_now_recv_info info{};
  info.src_addr = kAnswerer;
  EspNowV4Header h{};
  h.type = f.type;
  h.msgId = f.msgId;
  V4RxCtx ctx{};
  ctx.recv_info = &info;
  ctx.h = &h;
  ctx.payload = f.payload.data();
  ctx.payloadLen = static_cast<uint16_t>(f.payload.size());
  ctx.isPaired = ctx.isAuthenticated = ctx.isSessionEncrypted = true;
  ctx.deviceName = "peerB";
  if (f.type == ESPNOW_V4_TYPE_CMD_RESP) gLastReceiveVerdict = v4h_cmd_resp(ctx);
  else if (f.type == ESPNOW_V4_TYPE_STREAM) gLastReceiveVerdict = v4h_stream(ctx);
  else check(false, "receive: unexpected frame type");
}

static bool headerIs(const AirFrame& f, uint32_t reqId, uint8_t status) {
  uint32_t r = 0;
  uint8_t s = 0xFF;
  return espnowanswer::parseHeader(f.payload.data(), f.payload.size(), &r, &s) && r == reqId && s == status;
}
static bool ownFreshMsgId(const AirFrame& f, uint32_t reqId) {
  return f.msgId > 0x70000000u && f.msgId != reqId;
}

static void testRefusalIsAnAnswer() {
  resetWorld();
  check(v4_send_cmd_resp_async(kRequester, 41, espnowanswer::kBusy, "Error: busy"), "refusal: sent");
  check(air.size() == 1 && air[0].type == ESPNOW_V4_TYPE_CMD_RESP && (air[0].flags & ESPNOW_V4_FLAG_ACK_REQ),
        "refusal: one CMD_RESP with an ACK request");
  if (air.size() != 1) return;
  check(headerIs(air[0], 41, espnowanswer::kBusy), "refusal: payload starts {reqId 41, status busy}");
  check(ownFreshMsgId(air[0], 41), "refusal: header msgId is the answerer's own, not the CMD's");
  check(air[0].payload.size() == 5 + std::strlen("Error: busy") &&
            std::memcmp(air[0].payload.data() + 5, "Error: busy", 11) == 0,
        "refusal: the text follows the five bytes, no terminator");
  receive(air[0]);
  check(lastQueued() && lastQueued()->reqId == 41,
        "refusal received: correlated to request 41 by the payload's reqId, not the header msgId");
  check(lastQueued() && lastQueued()->status == espnowanswer::kBusy, "refusal received: not ok, status busy kept");
  check(lastQueuedText() == "Error: busy", "refusal received: the text");

  // A long refusal is cut to fit one frame, never past the 120-byte text room.
  resetWorld();
  const std::string longText(300, 'x');
  v4_send_cmd_resp_async(kRequester, 42, espnowanswer::kRefused, longText.c_str());
  check(air.size() == 1 && air[0].payload.size() == 5 + 120 && headerIs(air[0], 42, espnowanswer::kRefused),
        "long refusal: cut to 120 bytes of text after the header");
}

static void testResultIsAnAnswer() {
  resetWorld();
  const char* text = "uptime 5 min";
  check(v4_send_command_response(kRequester, 7, espnowanswer::kOk, text, (uint16_t)std::strlen(text)),
        "result: sent");
  check(air.size() == 1 && air[0].sync && air[0].type == ESPNOW_V4_TYPE_CMD_RESP, "result: one CMD_RESP");
  if (air.size() != 1) return;
  check(headerIs(air[0], 7, espnowanswer::kOk) && ownFreshMsgId(air[0], 7),
        "result: {reqId 7, status 0} under the answerer's own msgId");
  receive(air[0]);
  check(lastQueued() && lastQueued()->reqId == 7 && espnowanswer::isOk(lastQueued()->status) &&
            lastQueuedText() == text && std::strcmp(lastQueued()->name, "peerB") == 0,
        "result received: request 7, ok, the text, the sender's name");

  // A failure carries CMD_RESP's own 16 (failed); the receiver says so by name.
  resetWorld();
  v4_send_command_response(kRequester, 8, espnowanswer::kCmdFailed, "Error: nope", 11);
  receive(air.at(0));
  check(lastQueued() && lastQueued()->reqId == 8 && lastQueued()->status == espnowanswer::kCmdFailed,
        "failed result received: request 8, not ok, status 16");
  check(lastQueued() &&
            std::strcmp(espnowanswer::statusName(ESPNOW_V4_TYPE_CMD_RESP, lastQueued()->status), "failed") == 0,
        "failed result: status 16 is named 'failed' in CMD_RESP");

  // The receiver reads the correlation from the payload only: the same
  // answer under any header msgId files the same request.
  resetWorld();
  v4_send_command_response(kRequester, 9, espnowanswer::kOk, "a", 1);
  AirFrame copy = air.at(0);
  copy.msgId = 9999;
  receive(copy);
  check(lastQueued() && lastQueued()->reqId == 9,
        "the header msgId does not decide which request an answer is for");
}

// CMD_RESP burst (1.0 step 6): results wait in 16 entries with their texts in a
// 24 KB ring. The 17th, or one whose text does not fit the bytes left, is not
// taken: refused busy (the responder sends it once more under a new msgId),
// and nothing already queued is touched. Shown in arrival order; once the
// oldest are shown, there is room again (the ring wraps).
static void testCmdRespBurst() {
  resetWorld();
  for (uint32_t i = 0; i < espnowcmd::kResults; ++i) {
    air.clear();
    char text[32];
    std::snprintf(text, sizeof(text), "result %u", (unsigned)i);
    v4_send_command_response(kRequester, 600 + i, espnowanswer::kOk, text, (uint16_t)std::strlen(text));
    receive(air.at(0));
    check(gLastReceiveVerdict.kind == V4VerdictKind::Accepted, "burst: each of 16 results is taken");
  }
  check(gStore.results.count == espnowcmd::kResults, "burst: 16 results wait");
  air.clear();
  v4_send_command_response(kRequester, 700, espnowanswer::kOk, "one more", 8);
  receive(air.at(0));
  check(gLastReceiveVerdict.kind == V4VerdictKind::Refused && gLastReceiveVerdict.status == espnowanswer::kBusy &&
            gStore.results.count == espnowcmd::kResults,
        "burst: the 17th result is refused busy, nothing replaced");
  check(gStore.results.e[gStore.results.head].reqId == 600, "burst: the oldest waiting result is still the first");

  // Bytes: six 4 KB results fill the 24 KB ring; the seventh does not fit and
  // is refused busy although entries are free.
  resetWorld();
  const std::string big(4000, 'R');
  unsigned taken = 0, refusedBusy = 0;
  for (uint32_t i = 0; i < 7; ++i) {
    air.clear();
    v4_send_command_response(kRequester, 800 + i, espnowanswer::kOk, big.c_str(), (uint16_t)big.size());
    receive(air.at(0));
    if (gLastReceiveVerdict.kind == V4VerdictKind::Accepted) ++taken;
    else if (gLastReceiveVerdict.status == espnowanswer::kBusy) ++refusedBusy;
  }
  check(taken == 6 && refusedBusy == 1 && espnowcmd::bytesUsed(gStore.results) <= espnowcmd::kResultRingBytes,
        "burst: 4 KB results fill the 24 KB ring (6 fit), the next is refused busy");
  espnowcmd::pop(gStore.results);
  espnowcmd::pop(gStore.results);
  air.clear();
  v4_send_command_response(kRequester, 900, espnowanswer::kOk, big.c_str(), (uint16_t)big.size());
  receive(air.at(0));
  check(gLastReceiveVerdict.kind == V4VerdictKind::Accepted && lastQueued() && lastQueued()->off == 0 &&
            lastQueuedText() == big,
        "burst: after two are shown, the next result wraps to the ring's start whole");

  // No store (no PSRAM at start): every result is refused not ready (an
  // honest refusal, not busy: a retry would not help).
  resetWorld();
  gRemoteCmd = nullptr;
  v4_send_command_response(kRequester, 950, espnowanswer::kOk, "x", 1);
  receive(air.at(0));
  check(gLastReceiveVerdict.kind == V4VerdictKind::Refused && gLastReceiveVerdict.status == espnowanswer::kNotReady,
        "no result store: refused not ready");
  gRemoteCmd = &gStore;

  // The lab hook holds a result: it is queued with a later ready time.
  resetWorld();
  gRemoteCmdHoldMs = 40000;
  v4_send_command_response(kRequester, 960, espnowanswer::kOk, "held", 4);
  receive(air.at(0));
  check(lastQueued() && lastQueued()->readyAtMs == 1000 + 40000 && !espnowcmd::front(gStore.results, 1000) &&
            espnowcmd::front(gStore.results, 41000),
        "lab hold: the result is queued, shown only 40 s after it came");
}

static void testStreamLinesAreAnswers() {
  resetWorld();
  check(v4_send_stream_line(kRequester, 55, "line one", 8), "stream: sent");
  check(v4_send_stream_line(kRequester, 55, "line two", 8), "stream: second line sent");
  check(air.size() == 2 && air[0].type == ESPNOW_V4_TYPE_STREAM && air[1].type == ESPNOW_V4_TYPE_STREAM,
        "stream: two STREAM frames");
  if (air.size() != 2) return;
  check(headerIs(air[0], 55, 0) && headerIs(air[1], 55, 0), "stream: both start {reqId 55, status 0}");
  check(ownFreshMsgId(air[0], 55) && ownFreshMsgId(air[1], 55) && air[0].msgId != air[1].msgId,
        "stream: each line has a msgId of its own (the duplicate check applies to each)");
  receive(air[0]);
  receive(air[1]);
  check(gState.streamQueueHead == 2 && gQueue[0].cmdMsgId == 55 && gQueue[1].cmdMsgId == 55,
        "stream received: both lines filed under command 55 (the reqId), not their msgIds");
  check(streamExtended.size() == 2 && streamExtended[0] == 55 && streamExtended[1] == 55,
        "stream received: each line moves command 55's result deadline (E4, 1.0 step 6)");
  check(std::strcmp(gQueue[0].content, "line one") == 0 && std::strcmp(gQueue[1].content, "line two") == 0,
        "stream received: the text after the five bytes");

  // startstream output answers nothing: reqId 0.
  resetWorld();
  v4_send_stream_line(kRequester, 0, "live", 4);
  check(air.size() == 1 && headerIs(air[0], 0, 0) && air[0].msgId != 0,
        "startstream line: reqId 0, and still a msgId of its own (no 0 dedup skip)");

  // A line is cut to one frame for its path, the header included.
  resetWorld();
  const std::string longLine(400, 'L');
  v4_send_stream_line(kRequester, 9, longLine.c_str(), longLine.size());
  check(air.size() == 1 && air[0].payload.size() == ESPNOW_V4_MAX_PLAINTEXT, "long line direct: 202 bytes in all");
  resetWorld();
  frameCap = 146;
  v4_send_stream_line(kRequester, 9, longLine.c_str(), longLine.size());
  check(air.size() == 1 && air[0].payload.size() == 146 && headerIs(air[0], 9, 0),
        "long line routed: 146 bytes in all, header first");

  // A STREAM frame with only the header carries no line: nothing is filed.
  resetWorld();
  AirFrame empty{};
  empty.type = ESPNOW_V4_TYPE_STREAM;
  empty.msgId = 5;
  empty.payload.assign(5, 0);
  espnowanswer::packHeader(empty.payload.data(), 3, 0);
  receive(empty);
  check(gState.streamQueueHead == 0, "a header-only STREAM files nothing");

  // A full stream ring: the line is dropped, counted and answered busy (1.0 step 5b).
  resetWorld();
  v4_send_stream_line(kRequester, 7, "x", 1);
  gState.streamQueueTail = 0;
  gState.streamQueueHead = 15;  // one short of the tail: full
  receive(air[0]);
  check(gLastReceiveVerdict.kind == V4VerdictKind::Refused && gLastReceiveVerdict.status == espnowanswer::kBusy &&
            gState.streamQueueHead == 15,
        "a STREAM line into a full ring is refused busy and not filed");
  gState.streamQueueHead = gState.streamQueueTail = 0;
}

// A CMD frame as the dispatcher hands it to v4h_cmd.
static V4Verdict gLastCmdVerdict{};
// The CMD payload (E29): an account credential block, then the command.
static std::string cmdBlock(const char* user, const char* password, const char* command) {
  uint8_t buf[256];
  const size_t n = espnowcred::packCmd(buf, sizeof(buf), espnowcred::kKindAccount,
                                       reinterpret_cast<const uint8_t*>(user), std::strlen(user),
                                       reinterpret_cast<const uint8_t*>(password), std::strlen(password), command,
                                       std::strlen(command));
  return std::string(reinterpret_cast<const char*>(buf), n);
}

static void receiveCmd(const uint8_t* from, uint32_t msgId, const std::string& payload, bool sealed = true) {
  esp_now_recv_info info{};
  info.src_addr = const_cast<uint8_t*>(from);
  EspNowV4Header h{};
  h.type = ESPNOW_V4_TYPE_CMD;
  h.msgId = msgId;
  V4RxCtx ctx{};
  ctx.recv_info = &info;
  ctx.h = &h;
  ctx.payload = reinterpret_cast<const uint8_t*>(payload.data());
  ctx.payloadLen = static_cast<uint16_t>(payload.size());
  ctx.isPaired = ctx.isAuthenticated = true;
  ctx.isSessionEncrypted = sealed;
  ctx.deviceName = "peerA";
  gLastCmdVerdict = v4h_cmd(ctx);
}

// The answer on the air is {reqId, status} under a msgId of the answerer's
// own, and the requester files it under reqId with that status by name.
static void checkAnswer(const char* what, uint32_t reqId, uint8_t status, const char* name, bool ok) {
  std::string w = what;
  check(air.size() == 1 && air[0].type == ESPNOW_V4_TYPE_CMD_RESP, (w + ": one CMD_RESP").c_str());
  if (air.size() != 1) return;
  check(headerIs(air[0], reqId, status), (w + ": {reqId = the CMD's msgId, status}").c_str());
  check(ownFreshMsgId(air[0], reqId), (w + ": header msgId is the answerer's own").c_str());
  espnowcmd::queueClear(gStore.results);
  receive(air[0]);
  check(lastQueued() && lastQueued()->reqId == reqId && espnowanswer::isOk(lastQueued()->status) == ok &&
            lastQueued()->status == status,
        (w + ": received under the CMD's number with its status").c_str());
  check(std::strcmp(espnowanswer::statusName(ESPNOW_V4_TYPE_CMD_RESP, status), name) == 0,
        (w + ": the status is named " + name).c_str());
}

// v4h_cmd: one command waits in the slot; another CMD is answered busy by its
// ACK alone (1.0 step 6: no CMD_RESP(busy)).
static void testCmdSlotBusy() {
  resetWorld();
  const std::string first = cmdBlock("admin", "p:w", "status");
  receiveCmd(kRequester, 300, first);
  check(air.empty() && gState.deferredCmdPending && gState.deferredCmdMsgId == 300 &&
            gState.deferredCmdPayloadLen == first.size() &&
            std::memcmp(gState.deferredCmdPayload, first.data(), first.size()) == 0 &&
            gState.deferredCmdPayload[first.size()] == '\0',
        "slot free: the CMD (credential block + command, then a NUL) waits for the task, nothing is answered");
  check(gLastCmdVerdict.kind == V4VerdictKind::Accepted, "slot free: the CMD is accepted (its ACK says 0)");
  receiveCmd(kRequester, 300, first);
  check(air.empty(), "the waiting command again: no answer (it will run)");
  check(gLastCmdVerdict.kind == V4VerdictKind::Accepted, "the waiting command again: accepted (it runs once)");
  receiveCmd(kRequester, 301, cmdBlock("admin", "pw", "uptime"));
  check(gLastCmdVerdict.kind == V4VerdictKind::Refused && gLastCmdVerdict.status == espnowanswer::kBusy,
        "slot taken: the CMD is refused busy (its ACK says busy)");
  check(air.empty(), "slot taken: no CMD_RESP(busy) on the air, the ACK is the one answer (1.0 step 6)");
  check(gState.deferredCmdMsgId == 300, "slot taken: the waiting command is untouched");

  // 1.0 step 9 (E29): a payload that is not a credential block is answered
  // bad_request on its ACK and never reaches the slot.
  resetWorld();
  receiveCmd(kRequester, 310, "admin:pw:status");
  check(gLastCmdVerdict.kind == V4VerdictKind::Refused && gLastCmdVerdict.status == espnowanswer::kBadRequest &&
            !gState.deferredCmdPending && air.empty(),
        "the pre-1.0 colon text: refused bad_request, nothing waits");
  std::string noCommand = cmdBlock("admin", "pw", "x");
  noCommand.pop_back();
  receiveCmd(kRequester, 311, noCommand);
  check(gLastCmdVerdict.kind == V4VerdictKind::Refused && gLastCmdVerdict.status == espnowanswer::kBadRequest,
        "a block with no command: refused bad_request");
}

// v4_handle_cmd after authentication: the refusals it sends itself.
static void testHandleCmdRefusals() {
  resetWorld();
  sessionFree = false;
  v4_handle_cmd_after_auth(kRequester, "peerA", 400, "status", "admin", String("status"));
  checkAnswer("no stream session", 400, espnowanswer::kNoSlot, "no_slot", false);
  check(queuedCallback == nullptr, "no stream session: nothing queued");

  resetWorld();
  queueTakes = false;
  v4_handle_cmd_after_auth(kRequester, "peerA", 401, "status", "admin", String("status"));
  checkAnswer("queue full", 401, espnowanswer::kBusy, "busy", false);
  check(sessionsEnded.size() == 1 && sessionsEnded[0] == 401, "queue full: the stream session is ended");

  resetWorld();
  queueTakes = false;
  inputMax = 4;
  v4_handle_cmd_after_auth(kRequester, "peerA", 402, "status", "admin", String("status"));
  checkAnswer("command too long", 402, espnowanswer::kTooLarge, "too_large", false);
}

// v4CmdResultCallback: the answer to every command that ran.
static void testCommandResults() {
  struct Case {
    const char* what;
    bool ok;
    const char* text;
    bool noMemory;
    uint8_t status;
    const char* name;
    const char* sent;
  };
  const Case cases[] = {
      {"result ok", true, "uptime 5 min", false, espnowanswer::kOk, "ok", "uptime 5 min"},
      {"result failed", false, "Error: no such sensor", false, espnowanswer::kCmdFailed, "failed",
       "Error: no such sensor"},
      {"result ok without text", true, "", false, espnowanswer::kOk, "ok", "status"},
      {"result failed without text", false, nullptr, false, espnowanswer::kCmdFailed, "failed",
       "Error: status failed without a reply. Check the peer's log."},
      {"result ok, no memory for it", true, "uptime 5 min", true, espnowanswer::kOk, "ok", "status"},
      {"result failed, no memory for it", false, "Error: x", true, espnowanswer::kCmdFailed, "failed",
       kCommandReplyOutOfMemory},
  };
  uint32_t reqId = 500;
  for (const Case& c : cases) {
    resetWorld();
    ++reqId;
    v4_handle_cmd_after_auth(kRequester, "peerA", reqId, "status --all", "admin", String("status --all"));
    check(air.empty() && queuedCallback == v4CmdResultCallback && queuedUserData && queuedId == reqId,
          (std::string(c.what) + ": queued with the result callback under the CMD's number").c_str());
    if (!queuedCallback || !queuedUserData) continue;
    allocFails = c.noMemory;
    queuedCallback(c.ok, c.text, queuedUserData);  // as cmd_exec does
    allocFails = false;
    check(air.size() == 1 && air[0].sync, (std::string(c.what) + ": sent from cmd_exec, blocking").c_str());
    check(ackedSends.size() == 1 && ackedSends[0],
          (std::string(c.what) + ": sent waiting for its ACK (busy: sent once more, 1.0 step 6)").c_str());
    checkAnswer(c.what, reqId, c.status, c.name, c.ok);
    if (air.size() == 1) {
      const std::string text(air[0].payload.begin() + 5, air[0].payload.end());
      check(text == c.sent, (std::string(c.what) + ": the text after the five bytes").c_str());
    }
    check(!sessionsEnded.empty() && sessionsEnded.back() == reqId,
          (std::string(c.what) + ": the stream session is ended").c_str());
  }

  // No session up: the result goes the encrypt-or-wait way (queued until the
  // session opens), not waited for; a requester that stays busy is logged.
  resetWorld();
  v4_handle_cmd_after_auth(kRequester, "peerA", 590, "status", "admin", String("status"));
  sessionUp = false;
  if (queuedCallback) queuedCallback(true, "x", queuedUserData);
  check(air.size() == 1 && air[0].sync && ackedSends.empty() && headerIs(air[0], 590, espnowanswer::kOk),
        "no session: the result is handed to the encrypt-or-wait sender");
  resetWorld();
  v4_handle_cmd_after_auth(kRequester, "peerA", 591, "status", "admin", String("status"));
  chunkedResult = V4SendResult{V4SendOutcome::Refused, espnowanswer::kBusy};
  if (queuedCallback) queuedCallback(true, "x", queuedUserData);
  check(ackedSends.size() == 1 && warnings == 1, "requester busy after the retry: logged once");
}

// TOPO_START / TOPO_PEER from the answerer, as the dispatcher hands them on.
template <typename T>
static void receiveTopo(uint8_t type, const T& payload) {
  esp_now_recv_info info{};
  info.src_addr = kAnswerer;
  EspNowV4Header h{};
  h.type = type;
  h.msgId = generateMessageId();
  V4RxCtx ctx{};
  ctx.recv_info = &info;
  ctx.h = &h;
  ctx.payload = reinterpret_cast<const uint8_t*>(&payload);
  ctx.payloadLen = static_cast<uint16_t>(sizeof(T));
  ctx.isPaired = ctx.isAuthenticated = ctx.isSessionEncrypted = true;
  ctx.deviceName = "peerB";
  if (type == ESPNOW_V4_TYPE_TOPO_START) v4h_topo_start(ctx);
  else v4h_topo_peer(ctx);
}

static void resetTopo() {
  topoSlot = TopologyStream{};
  topoSlotUsed = false;
  topoFinalized = 0;
  gTopoRequestId = 77;
  gTopoRequestTimeout = 5000;  // millis() is 1000
}

// Rule F3 for the topology answers: a status that is not ok (busy, or one a
// later firmware adds) carries no topology, so nothing is recorded.
static void testTopologyStatus() {
  resetTopo();
  V4PayloadTopoStart start{};
  start.reqId = 77;
  start.status = espnowanswer::kBusy;
  start.peerCount = 0;
  receiveTopo(ESPNOW_V4_TYPE_TOPO_START, start);
  check(!topoSlotUsed && topoFinalized == 0,
        "TOPO_START busy with 0 peers: no stream, not recorded as an edge device");
  start.status = 200;  // a status this firmware does not know
  receiveTopo(ESPNOW_V4_TYPE_TOPO_START, start);
  check(!topoSlotUsed && topoFinalized == 0, "TOPO_START with an unknown status: nothing recorded");

  start.status = espnowanswer::kOk;
  start.peerCount = 1;
  receiveTopo(ESPNOW_V4_TYPE_TOPO_START, start);
  check(topoSlotUsed && topoSlot.started && topoSlot.totalPeers == 1 && topoFinalized == 0,
        "TOPO_START ok with one peer: the stream waits for it");
  V4PayloadTopoPeer peer{};
  peer.reqId = 77;
  peer.status = 16;  // not ok
  peer.peerIndex = 0;
  peer.isLast = 1;
  peer.mac[0] = 0x02;
  peer.mac[5] = 0x0C;
  receiveTopo(ESPNOW_V4_TYPE_TOPO_PEER, peer);
  check(topoSlot.receivedPeers == 0 && !topoSlot.peers[0].present && topoFinalized == 0,
        "TOPO_PEER not ok: no peer stored");
  peer.status = espnowanswer::kOk;
  receiveTopo(ESPNOW_V4_TYPE_TOPO_PEER, peer);
  check(topoSlot.receivedPeers == 1 && topoSlot.peers[0].present && topoSlot.peers[0].mac[5] == 0x0C &&
            topoFinalized == 1,
        "TOPO_PEER ok: stored, and the stream completes");

  resetTopo();
  start.status = espnowanswer::kOk;
  start.peerCount = 0;
  receiveTopo(ESPNOW_V4_TYPE_TOPO_START, start);
  check(topoSlotUsed && topoFinalized == 1, "TOPO_START ok with 0 peers: an edge device");
}

// The web bond page's FS error tags are the one name table's words (rule
// F3): busy and refused, never a second set of names.
static void testWebFsTags() {
  bool same = true;
  for (unsigned st = 0; st < 256; ++st) {
    const char* name = espnowanswer::statusName(ESPNOW_V4_TYPE_FS_LIST_RESP, (uint8_t)st);
    same = same && std::strcmp(fsStatusTag((uint8_t)st), name ? name : "unknown") == 0;
  }
  check(same, "web FS tags: every status named as the shared table names it");
  check(std::strcmp(fsStatusTag(FS_LIST_STATUS_TOO_BUSY), "busy") == 0 &&
            std::strcmp(fsStatusTag(FS_LIST_STATUS_PERM_DENIED), "refused") == 0 &&
            std::strcmp(fsStatusTag(FS_LIST_STATUS_NOT_A_DIR), "not_a_dir") == 0 &&
            std::strcmp(fsStatusTag(200), "unknown") == 0,
        "web FS tags: busy, refused, not_a_dir, unknown");
}

int main() {
  testWebFsTags();
  testTopologyStatus();
  testCmdSlotBusy();
  testHandleCmdRefusals();
  testCommandResults();
  testRefusalIsAnAnswer();
  testResultIsAnAnswer();
  testCmdRespBurst();
  testStreamLinesAreAnswers();
  std::printf("espnow_answers harness: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
