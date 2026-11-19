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
// answer), v4CmdResultCallback (every remote command's result) and
// v4_handle_cmd from its authentication decision on. The stream-session
// table, the command queue and the executor are doubles; the executor's
// callback is called by the cases as cmd_exec would.
#include <Arduino.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_ESPNow_RxCtx.h"  // V4RxCtx over the real System_ESPNow_Wire.h
#include "System_DeviceName.h"

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
#define WARN_ESPNOWF(...) do {} while (0)
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
  uint32_t deferredCmdMsgId = 0;
  bool deferredCmdWasEncrypted = false;
  bool deferredCmdRespPending = false;
  uint8_t deferredCmdRespSrcMac[6] = {};
  char deferredCmdRespDeviceName[32] = {};
  char* deferredCmdRespResult = nullptr;
  bool deferredCmdRespSuccess = false;
  uint8_t deferredCmdRespStatus = 0;
  uint32_t deferredCmdRespReqId = 0;
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
static char gRespBuffer[V4_REASM_MAX_MSG + 1];
static EspNowState::StreamQueueEntry gQueue[16];

// ---- production -------------------------------------------------------------------------
// INSERT_PRODUCTION

// ---- cases --------------------------------------------------------------------------------
static uint8_t kRequester[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0A};
static uint8_t kAnswerer[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0B};

static void resetWorld() {
  air.clear();
  broadcasts.clear();
  gState = EspNowState{};
  gState.deferredCmdRespResult = gRespBuffer;
  std::memset(gRespBuffer, 0, sizeof(gRespBuffer));
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
  if (f.type == ESPNOW_V4_TYPE_CMD_RESP) v4h_cmd_resp(ctx);
  else if (f.type == ESPNOW_V4_TYPE_STREAM) v4h_stream(ctx);
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
  check(gState.deferredCmdRespPending && gState.deferredCmdRespReqId == 41,
        "refusal received: correlated to request 41 by the payload's reqId, not the header msgId");
  check(!gState.deferredCmdRespSuccess && gState.deferredCmdRespStatus == espnowanswer::kBusy,
        "refusal received: not ok, status busy kept");
  check(std::strcmp(gState.deferredCmdRespResult, "Error: busy") == 0, "refusal received: the text");

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
  check(gState.deferredCmdRespReqId == 7 && gState.deferredCmdRespSuccess &&
            std::strcmp(gState.deferredCmdRespResult, text) == 0,
        "result received: request 7, ok, the text");

  // A failure carries CMD_RESP's own 16 (failed); the receiver says so by name.
  resetWorld();
  v4_send_command_response(kRequester, 8, espnowanswer::kCmdFailed, "Error: nope", 11);
  receive(air.at(0));
  check(gState.deferredCmdRespReqId == 8 && !gState.deferredCmdRespSuccess &&
            gState.deferredCmdRespStatus == espnowanswer::kCmdFailed,
        "failed result received: request 8, not ok, status 16");
  check(std::strcmp(espnowanswer::statusName(ESPNOW_V4_TYPE_CMD_RESP, gState.deferredCmdRespStatus), "failed") == 0,
        "failed result: status 16 is named 'failed' in CMD_RESP");

  // The receiver reads the correlation from the payload only: the same
  // answer under any header msgId files the same request.
  resetWorld();
  v4_send_command_response(kRequester, 9, espnowanswer::kOk, "a", 1);
  AirFrame copy = air.at(0);
  copy.msgId = 9999;
  receive(copy);
  check(gState.deferredCmdRespReqId == 9, "the header msgId does not decide which request an answer is for");
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
}

// A CMD frame as the dispatcher hands it to v4h_cmd.
static void receiveCmd(const uint8_t* from, uint32_t msgId, const char* text, bool sealed = true) {
  esp_now_recv_info info{};
  info.src_addr = const_cast<uint8_t*>(from);
  EspNowV4Header h{};
  h.type = ESPNOW_V4_TYPE_CMD;
  h.msgId = msgId;
  V4RxCtx ctx{};
  ctx.recv_info = &info;
  ctx.h = &h;
  ctx.payload = reinterpret_cast<const uint8_t*>(text);
  ctx.payloadLen = static_cast<uint16_t>(std::strlen(text));
  ctx.isPaired = ctx.isAuthenticated = true;
  ctx.isSessionEncrypted = sealed;
  ctx.deviceName = "peerA";
  v4h_cmd(ctx);
}

// The answer on the air is {reqId, status} under a msgId of the answerer's
// own, and the requester files it under reqId with that status by name.
static void checkAnswer(const char* what, uint32_t reqId, uint8_t status, const char* name, bool ok) {
  std::string w = what;
  check(air.size() == 1 && air[0].type == ESPNOW_V4_TYPE_CMD_RESP, (w + ": one CMD_RESP").c_str());
  if (air.size() != 1) return;
  check(headerIs(air[0], reqId, status), (w + ": {reqId = the CMD's msgId, status}").c_str());
  check(ownFreshMsgId(air[0], reqId), (w + ": header msgId is the answerer's own").c_str());
  gState.deferredCmdRespPending = false;
  receive(air[0]);
  check(gState.deferredCmdRespPending && gState.deferredCmdRespReqId == reqId &&
            gState.deferredCmdRespSuccess == ok && gState.deferredCmdRespStatus == status,
        (w + ": received under the CMD's number with its status").c_str());
  check(std::strcmp(espnowanswer::statusName(ESPNOW_V4_TYPE_CMD_RESP, status), name) == 0,
        (w + ": the status is named " + name).c_str());
}

// v4h_cmd: one command waits in the slot; another sealed CMD is answered busy.
static void testCmdSlotBusy() {
  resetWorld();
  receiveCmd(kRequester, 300, "admin:pw:status");
  check(air.empty() && gState.deferredCmdPending && gState.deferredCmdMsgId == 300 &&
            std::strcmp(gState.deferredCmdPayload, "admin:pw:status") == 0,
        "slot free: the CMD waits for the task, nothing is answered");
  receiveCmd(kRequester, 300, "admin:pw:status");
  check(air.empty(), "the waiting command again: no answer (it will run)");
  receiveCmd(kRequester, 301, "admin:pw:status", false);
  check(air.empty(), "an unsealed CMD while the slot is taken: no answer");
  receiveCmd(kRequester, 301, "admin:pw:uptime");
  checkAnswer("slot taken", 301, espnowanswer::kBusy, "busy", false);
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
    checkAnswer(c.what, reqId, c.status, c.name, c.ok);
    if (air.size() == 1) {
      const std::string text(air[0].payload.begin() + 5, air[0].payload.end());
      check(text == c.sent, (std::string(c.what) + ": the text after the five bytes").c_str());
    }
    check(!sessionsEnded.empty() && sessionsEnded.back() == reqId,
          (std::string(c.what) + ": the stream session is ended").c_str());
  }
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
    const char* name = espnowanswer::statusName(ESPNOW_V4_TYPE_FS_LIST_REPLY, (uint8_t)st);
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
  testStreamLinesAreAnswers();
  std::printf("espnow_answers harness: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
