// Mock transport/platform boundaries around extracted SHIPPING owner and CLI.
#include "G2_ConversateSession.h"
#include <assert.h>
#include <stdio.h>
#include <sstream>
#include <string>
#include <vector>

struct G2Temple {
  char side; uint32_t connectionGeneration; bool connected; bool audioNotifyChar;
  uint8_t* rxBuf = nullptr;
  size_t rxFrameHave = 0, rxFrameExpected = 0;
  uint32_t rxFrameConversateEpoch = 0;
  G2RxReassembly rxReassembly{};
  uint32_t rxStartedMs = 0, rxReassemblyGeneration = 0;
  uint32_t rxReassemblyLifecycleEpoch = 0, rxReassemblyPresentationEpoch = 0;
  uint32_t rxReassemblyConversateEpoch = 0, packetsReceived = 0;
};
static G2Temple gL{'L', 1, true, true}, gR{'R', 2, true, true};
static G2ConversateSession gConversate;
static portMUX_TYPE gConversateMux = 0;
static int32_t gConversateRequest = 0;
static bool gConversateFastHeld = false, gConversateDeferredPrep = false;
static uint32_t gConversateMagic = 0, gConversateDeferredMagic = 0, gConversateRxEpoch = 1;
static uint32_t gConversatePrepMagic = 0, gConversateStartMagic = 0;
static portMUX_TYPE gEvenAiSessionMux = 0;
static uint32_t gEvenAiMagicCtr = 200;
static bool gMicStreamOn = false, gMicProbeActive = false;
static bool gMicRecFile = false, gMicWavFile = false;
static bool capture = false, evenAi = false, lens = false, hijack = false;
static bool txOk = true;
static int fastDepth = 0;
static std::vector<std::vector<uint8_t>> sent;
static char lastSentSide = 0;
static G2ConversateSession g2ConversateSnapshot() { return gConversate; }
static bool nativeCapture = false, nativeArmed = false, nativeFailed = false;
static bool nativePaused = false, nativeFinishedClean = false;
static uint64_t nativeExchange = 0;
static bool audioCaptureBusy() { return capture || nativeCapture; }
static bool audioCaptureOwnedBy(const char*) { return nativeCapture; }
enum class LiveAudioConversateAdmission : uint8_t { Disabled, Started, Failed };
static LiveAudioConversateAdmission liveAudioConversateBegin(uint32_t gen, uint64_t* id) {
  assert(gen == gL.connectionGeneration); *id = 0;
  if (!nativeArmed) return LiveAudioConversateAdmission::Disabled;
  if (nativeFailed) return LiveAudioConversateAdmission::Failed;
  nativeCapture = true; nativePaused = false; *id = ++nativeExchange;
  return LiveAudioConversateAdmission::Started;
}
static bool liveAudioConversateRunning(uint64_t id) {
  return nativeCapture && id == nativeExchange && !nativeFailed;
}
static bool liveAudioConversatePending(uint64_t id) {
  return nativeCapture && id == nativeExchange;
}
static void liveAudioConversatePause(uint64_t id, bool paused) {
  if (liveAudioConversateRunning(id)) nativePaused = paused;
}
static void liveAudioConversateFinish(uint64_t id, bool clean) {
  if (id && id == nativeExchange) {
    nativeFinishedClean = clean;
    nativeCapture = false;
  }
}
static bool g2EvenAiSessionIsActive() { return evenAi; }
struct Lens { bool containerReady; };
static Lens g2LensGetState() { return {lens}; }
static bool g2FsmHijackActive() { return hijack; }
static bool g2TempleReadyAtGeneration(const G2Temple& t, uint32_t generation = 0) {
  return t.connected && (!generation || generation == t.connectionGeneration);
}
static uint8_t allocSeq() { static uint8_t seq = 0; return ++seq; }
static bool sendEnvelopeAtGeneration(G2Temple& t, const uint8_t* frame,
                                     size_t n, uint32_t generation) {
  assert((t.side == 'R' && frame[6] == G2_SID_CONVERSATE) || frame[6] == G2_SID_EVEN_AI);
  assert(generation == t.connectionGeneration);
  assert(gConversate.active()); // includes the complete asynchronous CLOSE lease
  if (txOk) { sent.emplace_back(frame, frame + n); lastSentSide = t.side; }
  return txOk;
}
static void g2ConnPriRequestFast(const char*) { ++fastDepth; }
static void g2ConnPriReleaseFast(const char*) { --fastDepth; assert(fastDepth >= 0); }
static bool g2LinkIsSlow(const G2Temple*) { return false; }
static void g2ConnPriReapply() {}
static void g2ControlWake() {}
static bool g2ControlWorkerEnsure() { return true; }
static void debugStub(const char*, ...) {}
#define DEBUG_G2F(...) debugStub(__VA_ARGS__)
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
using String = std::string;
class CommandArgs {
  std::vector<String> parts;
 public:
  explicit CommandArgs(const String& s) {
    std::istringstream input(s); String part;
    while (input >> part) parts.push_back(part);
  }
  size_t count() const { return parts.size(); }
  String arg(size_t i) const { return parts.at(i); }
};
static void g2ConversateStop(G2ConversateSession::Stop reason, uint32_t replyMagic = 0);
static void g2ConversateTick(bool requestsOnly = false);
static void g2ConversateOnRx(G2Temple&, uint32_t, const uint8_t*, size_t);
static std::vector<uint8_t> unhex(const char* hex);

// INSERT_PRODUCTION_OWNER

static constexpr size_t RX_FRAME_CAP = G2_ENVELOPE_HDR_LEN + 255;
static constexpr size_t RX_MESSAGE_CAP = 1024;
static constexpr size_t G2_BLE_LOCAL_MTU_PREF = 517;
static portMUX_TYPE gRxPacketMux = 0;
static uint32_t gWidgetLifecycleEpoch = 1;
static uint32_t g2PresentationEpochCurrent() { return 1; }
static void g2RingRecord(char, char, const uint8_t*, size_t) {}
static unsigned unrelatedDispatches = 0;
static void g2DispatchCompleteEnvelope(G2Temple& t, const G2EnvelopeView& env,
                                       uint32_t generation, uint32_t, uint32_t) {
  if (env.sid == G2_SID_CONVERSATE) g2ConversateOnRx(t,generation,env.payload,env.payloadLen);
  else ++unrelatedDispatches;
}
// INSERT_PRODUCTION_RX

using CS = G2ConversateSession;
static void rx(uint32_t command, uint32_t magic, uint32_t field,
               std::initializer_list<uint8_t> body = {}) {
  uint8_t pb[96]; size_t n = 0;
  assert(g2PbWriteUint32(pb, sizeof(pb), &n, 1, command));
  assert(g2PbWriteUint32(pb, sizeof(pb), &n, 2, magic));
  assert(g2PbWriteBytes(pb, sizeof(pb), &n, field, body.begin(), body.size()));
  g2ConversateOnRx(gR, gR.connectionGeneration, pb, n);
}
static void ack(uint32_t magic, uint8_t error = 0) { rx(162, magic, 10, {8,error}); }
static void finishClose() {
  assert(gConversate.phase == CS::Phase::Closing && fastDepth == 0);
  if (gConversate.closeAckSafe) ack(gConversate.closeMagic);
  else { clockMs += CS::CloseTimeoutMs; g2ConversateTick(); }
  assert(!gConversate.active());
}
static void stopRun() {
  gConversateRequest = -1; g2ConversateTick(true);
  if (gConversate.active()) finishClose();
}
static void startRun() {
  assert(gConversate.enabled && !gConversate.active());
  rx(2, 1, 4);
  assert(gConversate.phase == CS::Phase::Armed && gConversate.prepared);
  const auto size = sent.size();
  rx(2, 1, 4);
  assert(sent.size() == size); // duplicate PREP cannot mint unreserved tokens
  rx(4, 3, 6, {8,1});
  assert(gConversate.phase == CS::Phase::Running && fastDepth == 1);
  rx(4, 3, 6, {8,1});
  assert(sent.size() == size + 1 && fastDepth == 1);
}
static void liveBeat(uint8_t error = 0) {
  clockMs += 5000;
  gConversate.audio(clockMs, gL.connectionGeneration, uint8_t(gConversate.packets));
  g2ConversateTick();
  assert(gConversate.heartbeatPending);
  ack(gConversate.pendingMagic, error);
}
static void ownerTests() {
  rx(2,1,4); rx(4,3,6,{8,1});
  assert(!gConversate.active() && sent.empty());
  assert(strstr(cmd_g2conversate("on 30"), "Usage:"));
  for (const char* arg : {"test 0", "test 9", "test 3601", "test -1", "test 1x", "test 99999"})
    assert(strstr(cmd_g2conversate(arg), "Error:"));
  assert(gConversateRequest == 0);
  cmd_g2conversate("on"); assert(gConversateRequest == -3);
  g2ConversateTick(true);
  assert(gConversate.enabled && gConversate.limitSeconds == 0 && fastDepth == 0);
  clockMs += 120000; g2ConversateTick(); rx(4,3,6,{8,1});
  assert(!gConversate.active() && sent.empty());
  const uint8_t prep[] = {8,2,16,1,34,0};
  g2ConversateOnRx(gL,1,prep,sizeof(prep)); g2ConversateOnRx(gR,99,prep,sizeof(prep));
  assert(sent.empty()); startRun();
  assert(strstr(cmd_g2conversate("status"), "limit_s=0"));

  // Captured stock rejection near exit: it does NOT send immediate CLOSE.
  const auto before = sent.size(); liveBeat(1);
  assert(gConversate.phase == CS::Phase::Running && gConversate.consecutiveFailures == 1);
  assert(sent.size() == before + 1); liveBeat();
  assert(gConversate.consecutiveFailures == 0 && gConversate.heartbeatFailures == 1);
  const uint32_t oldHb = gConversate.pendingMagic;
  rx(161,160,9,{8,2});
  assert(gConversate.phase == CS::Phase::Closing && gConversate.reason == CS::Stop::NativeExit);
  const auto closingPackets = gConversate.packets;
  const auto closingSends = sent.size();
  gConversate.audio(clockMs,1,42);
  rx(4,3,6,{8,1}); rx(161,161,9,{8,4}); ack(oldHb); ack(160,1);
  assert(gConversate.phase == CS::Phase::Closing && gConversate.packets == closingPackets);
  assert(sent.size() == closingSends);
  // Fast reopen waits for close completion, then consumes a fresh PREP only.
  rx(2,162,4); assert(gConversateDeferredPrep && sent.size() == closingSends);
  finishClose(); g2ConversateTick();
  assert(gConversate.phase == CS::Phase::Armed && !gConversateDeferredPrep);
  rx(4,163,6,{8,1});
  assert(gConversate.phase == CS::Phase::Running && gConversate.packets == 0);
  stopRun(); assert(gConversate.enabled);

  // Heartbeats also run while preparing. SELECT may reuse the pending token;
  // its START ACK must not be credited as a heartbeat ACK.
  rx(2,1,4); clockMs += 5000; g2ConversateTick();
  assert(gConversate.phase == CS::Phase::Armed && gConversate.heartbeatPending);
  const auto preparingToken = gConversate.pendingMagic;
  rx(4,preparingToken,6,{8,1}); ack(preparingToken);
  assert(gConversate.phase == CS::Phase::Running && !gConversate.heartbeatPending);
  assert(gConversate.acknowledgements == 0); stopRun();

  rx(2,1,4); rx(164,135,14,{16,1}); rx(4,136,6,{8,1});
  uint8_t configuredStart[256];
  const auto configuredSize = g2BuildConversateControl(sent.back()[2],136,true,
      configuredStart,sizeof(configuredStart),false,true);
  assert(sent.back() == std::vector<uint8_t>(configuredStart,configuredStart+configuredSize));
  assert(!gConversate.transcribeVisible && gConversate.aiCueVisible); stopRun();

  // Visibility and translation packets from the second stock capture.
  startRun();
  struct Menu { const char* hex; bool transcription, cues; };
  for (const auto& menu : {Menu{"08a40110870172021001",false,true},
                          Menu{"08a401108a01720408011001",true,true},
                          Menu{"08a401108c0172020801",true,false},
                          Menu{"08a401108e01720408011001",true,true}}) {
    auto pb = unhex(menu.hex); g2ConversateOnRx(gR,2,pb.data(),pb.size());
    assert(gConversate.phase == CS::Phase::Running && fastDepth == 1);
    assert(gConversate.transcribeVisible == menu.transcription && gConversate.aiCueVisible == menu.cues);
    G2ConversateEvent event; assert(g2ParseConversateEvent(pb.data(),pb.size(),&event));
    uint8_t expected[64]; auto n = g2BuildConversateInterfaceReply(sent.back()[2],event.magic,0,
        menu.transcription,menu.cues,expected,sizeof(expected));
    assert(sent.back() == std::vector<uint8_t>(expected,expected+n));
  }
  assert(gConversate.interfaceChanges == 4);
  rx(164,143,14,{8,2});
  assert(gConversate.transcribeVisible && gConversate.aiCueVisible && gConversate.interfaceChanges == 4);
  for (const auto& key : {std::string("AUTO"),std::string("ES"),std::string("FR"),std::string("OFF")}) {
    uint8_t pb[64], nested[24], expected[64]; size_t n = 0, inner = 0;
    g2PbWriteString(nested,sizeof(nested),&inner,1,key.c_str());
    g2PbWriteUint32(pb,sizeof(pb),&n,1,166); g2PbWriteUint32(pb,sizeof(pb),&n,2,147);
    g2PbWriteBytes(pb,sizeof(pb),&n,16,nested,inner); g2ConversateOnRx(gR,2,pb,n);
    const auto len = g2BuildConversateLanguageReply(sent.back()[2],147,key=="OFF" ? 0 : 1,expected,sizeof(expected));
    assert(sent.back() == std::vector<uint8_t>(expected,expected+len));
    assert(gConversate.phase == CS::Phase::Running);
  }
  assert(gConversate.languageRequests == 4);
  // A reused menu token must not fake heartbeat liveness.
  clockMs += 5000; gConversate.audio(clockMs,1,1); g2ConversateTick();
  const auto collision = gConversate.pendingMagic, successes = gConversate.acknowledgements;
  rx(164,collision,14,{8,1,16,1}); ack(collision);
  assert(!gConversate.heartbeatPending && gConversate.acknowledgements == successes);
  liveBeat(); assert(gConversate.pendingMagic != collision && gConversate.consecutiveFailures == 0);

  // Actual PAUSE/RESUME (APK-derived, not the physical display-off action).
  rx(161,180,9,{8,3}); assert(gConversate.phase == CS::Phase::Paused);
  const auto pausedPackets = gConversate.packets, gap = gConversate.maxGapMs;
  for (int i=0; i<15; ++i) liveBeat();
  assert(gConversate.phase == CS::Phase::Paused && gConversate.packets == pausedPackets);
  rx(161,181,9,{8,4}); assert(gConversate.phase == CS::Phase::Running);
  const auto resumeGrace = gConversate.audioExpectedSinceMs;
  clockMs += 100; rx(161,181,9,{8,4});
  assert(gConversate.audioExpectedSinceMs == resumeGrace); // idempotent response, no watchdog extension
  clockMs += 1000; gConversate.audio(clockMs,1,200); g2ConversateTick();
  assert(gConversate.packets == pausedPackets+1 && gConversate.maxGapMs == gap);
  rx(168,182,18); assert(gConversate.phase == CS::Phase::Closing); finishClose();
  rx(161,183,9,{8,4}); assert(!gConversate.active());

  // Close timeout, deferred request cancellation, and reconnection fences.
  startRun(); rx(161,190,9,{8,2}); rx(2,191,4); rx(168,192,18);
  assert(!gConversateDeferredPrep); clockMs += CS::CloseTimeoutMs; g2ConversateTick();
  assert(!gConversate.active() && gConversate.closeTimedOut);
  startRun(); rx(161,193,9,{8,2}); rx(2,194,4);
  ++gL.connectionGeneration; g2ConversateTick();
  assert(!gConversate.active() && !gConversateDeferredPrep); --gL.connectionGeneration;
  startRun(); const auto beforeDisconnect = sent.size(); ++gR.connectionGeneration;
  g2ConversateTick(); assert(!gConversate.active() && sent.size() == beforeDisconnect && fastDepth == 0);
  g2ConversateOnRx(gR,2,prep,sizeof(prep)); assert(!gConversate.active());
  startRun(); stopRun(); --gR.connectionGeneration;

  // Bounded failure budget; the independent audio watchdog still wins silence.
  startRun();
  for (uint32_t i=1; i<=CS::MaxHeartbeatFailures; ++i) {
    liveBeat(1); assert(gConversate.phase == CS::Phase::Running);
    if (i<CS::MaxHeartbeatFailures) { g2ConversateTick(); assert(gConversate.live()); }
  }
  g2ConversateTick(); assert(gConversate.reason == CS::Stop::HeartbeatLost); finishClose();
  startRun(); liveBeat(); clockMs += CS::AudioTimeoutMs; g2ConversateTick();
  assert(gConversate.reason == CS::Stop::AudioStall); finishClose();
  startRun(); rx(161,200,9,{8,3});
  for (uint32_t i=0; i<CS::MaxHeartbeatFailures; ++i) {
    clockMs += CS::AckTimeoutMs; g2ConversateTick();
  }
  clockMs += CS::AckTimeoutMs; g2ConversateTick();
  assert(gConversate.reason == CS::Stop::HeartbeatLost && gConversate.ackTimeouts == 12); finishClose();

  // Competing capture/UI owners are refused without changing their state.
  const auto beforeBusy = sent.size();
  for (bool* owner : {&capture,&gMicStreamOn,&gMicProbeActive,&gMicRecFile,&gMicWavFile,&evenAi,&lens,&hijack}) {
    *owner = true; rx(2,1,4);
    assert(!gConversate.active() && sent.size() == beforeBusy && fastDepth == 0);
    *owner = false;
  }
  assert(!g2ConversateDeclineEvenAi(gR,2)); startRun();
  const auto beforeWake = sent.size();
  assert(g2ConversateDeclineEvenAi(gR,0) && g2ConversateDeclineEvenAi(gR,999));
  assert(sent.size() == beforeWake);
  for (G2Temple* t : {&gL,&gR}) {
    assert(g2ConversateDeclineEvenAi(*t,t->connectionGeneration));
    assert(lastSentSide == t->side && gConversate.phase == CS::Phase::Running && !evenAi);
  }
  rx(161,200,9,{8,3}); assert(g2ConversateDeclineEvenAi(gR,2));
  assert(gConversate.phase == CS::Phase::Paused); stopRun();
  startRun(); txOk = false; clockMs += 5000; gConversate.audio(clockMs,1,0); g2ConversateTick();
  assert(!gConversate.active() && gConversate.reason == CS::Stop::TxFailed && fastDepth == 0);
  txOk = true;

  // Two simulated hours cross millis wrap and multiple native token rotations.
  clockMs = UINT32_MAX - 10000; gConversateMagic = 249;
  startRun(); const uint32_t began = clockMs; uint32_t priorMagic = 0;
  for (uint32_t elapsed=0; elapsed<7200000; elapsed+=50) {
    clockMs = began + elapsed; gConversate.audio(clockMs,1,uint8_t(elapsed/50)); g2ConversateTick();
    assert(gConversate.phase == CS::Phase::Running);
    if (gConversate.heartbeatPending) {
      const auto magic = gConversate.pendingMagic;
      assert(magic>0 && magic<=250 && magic!=priorMagic);
      ack(gConversatePrepMagic); ack(gConversateStartMagic); ack(priorMagic); ack(1000000);
      assert(gConversate.heartbeatPending && gConversateMagic == magic);
      ack(magic); priorMagic = magic;
    }
  }
  assert(gConversate.packets == 144000 && gConversate.heartbeats == 1439);
  assert(gConversate.acknowledgements == 1439 && gConversate.magicWraps >= 5);
  assert(gConversate.maxGapMs == 50 && !gConversate.lostPackets && !gConversate.duplicates);
  stopRun();
  cmd_g2conversate("test 10"); assert(gConversateRequest == 10); g2ConversateTick(true);
  startRun(); clockMs += 10000; g2ConversateTick();
  assert(gConversate.reason == CS::Stop::Deadline); finishClose();
  startRun(); assert(gConversate.durationMs == 10000); stopRun();
  cmd_g2conversate("test"); assert(gConversateRequest == 300); g2ConversateTick(true);
  rx(2,1,4); clockMs += CS::ArmTimeoutMs; g2ConversateTick();
  assert(gConversate.reason == CS::Stop::ArmTimeout); finishClose(); startRun();
  const auto epoch = gConversateRxEpoch;
  cmd_g2conversate("off"); cmd_g2conversate("stop"); assert(gConversateRequest == -2);
  g2ConversateTick(true); assert(gConversateRxEpoch != epoch && !gConversate.enabled); finishClose();
  rx(2,1,4); rx(4,3,6,{8,1}); assert(!gConversate.active());
  cmd_g2conversate("on"); g2ConversateTick(true); startRun();
  g2ConversateStop(CS::Stop::Shutdown);
  assert(!gConversate.enabled && !gConversate.active() && fastDepth == 0);
  cmd_g2conversate("on"); g2ConversateTick(true); g2ConversateStop(CS::Stop::Shutdown);
  assert(!gConversate.enabled && !gConversate.active());
}

static std::vector<uint8_t> notifyFrame(const char* hex, uint8_t sid = G2_SID_CONVERSATE) {
  const auto pb = unhex(hex); uint8_t frame[256];
  const auto n = g2BuildEnvelope(42,sid,G2_FLAG_NOTIFY,pb.data(),pb.size(),frame,sizeof(frame));
  assert(n); frame[1] = G2_PREAMBLE_RX;
  return {frame,frame+n};
}
static void drainRx() {
  G2RxPacket p;
  while (g2RxPacketDequeue(&p)) processNotify(gR,p.data,p.len,p.generation,p.lifecycleEpoch,p.presentationEpoch,p.conversateEpoch);
}
static void rxQueueTests() {
  static uint8_t storage[RX_MESSAGE_CAP + RX_FRAME_CAP];
  gR.rxBuf = storage; g2RxReassemblyInit(&gR.rxReassembly,storage,RX_MESSAGE_CAP);
  static G2RxPacket packets[G2_RX_PACKET_DEPTH], priority[G2_RX_EVENAI_CTRL_DEPTH];
  gRxPackets = packets; gRxEvenAiCtrlPackets = priority;
  const auto prep = notifyFrame("080210012200");
  const auto select = notifyFrame("0804100332020801");
  const auto close = notifyFrame("08a101109b014a020802");
  const auto pause = notifyFrame("08a101109b014a020803");
  const auto background = notifyFrame("08011a02080b",G2_SID_SYNC_INFO);
  assert(g2RxConversateTerminal(close.data(),close.size()) == 2);
  assert(g2RxConversateTerminal(pause.data(),pause.size()) == 3);
  auto corrupt = close; corrupt.back() ^= 1;
  assert(!g2RxConversateTerminal(corrupt.data(),corrupt.size()));
  corrupt = close; corrupt[1] = G2_PREAMBLE_TX;
  assert(!g2RxConversateTerminal(corrupt.data(),corrupt.size()));

  cmd_g2conversate("on"); g2ConversateTick(true);
  // A queued PREP/SELECT before local stop must not reopen after it.
  assert(g2RxPacketEnqueue(gR,prep.data(),prep.size()));
  assert(g2RxPacketEnqueue(gR,select.data(),select.size()));
  stopRun(); drainRx(); assert(!gConversate.active());
  // Also fence a raw envelope split across two BLE notifications.
  const uint32_t oldEpoch = gConversateRxEpoch;
  processNotify(gR,prep.data(),9,2,1,1,oldEpoch);
  stopRun();
  processNotify(gR,prep.data()+9,prep.size()-9,2,1,1,gConversateRxEpoch);
  assert(!gConversate.active());
  // Other protocols are not invalidated by Conversate's cancellation epoch.
  processNotify(gR,background.data(),background.size(),2,1,1,oldEpoch);
  assert(unrelatedDispatches == 1);

  // Expiring an abandoned fragmented message must not erase the new frame's
  // epoch and incorrectly suppress an otherwise fresh PREP.
  gR.rxReassembly.active = true;
  gR.rxStartedMs = clockMs - 3000;
  processNotify(gR,prep.data(),prep.size(),2,1,1,gConversateRxEpoch);
  assert(gConversate.phase == CS::Phase::Armed); stopRun();

  startRun();
  for (unsigned i=0; i<G2_RX_PACKET_DEPTH; ++i)
    assert(g2RxPacketEnqueue(gR,background.data(),background.size()));
  assert(g2RxPacketEnqueue(gR,close.data(),close.size()));
  assert(!g2RxPacketEnqueue(gR,pause.data(),pause.size())); // PAUSE cannot evict queued CLOSE
  assert(!g2RxPacketEnqueue(gR,background.data(),background.size()));
  drainRx(); assert(gConversate.phase == CS::Phase::Closing); finishClose();
  assert(unrelatedDispatches == G2_RX_PACKET_DEPTH);
  startRun();
  for (unsigned i=0; i<G2_RX_PACKET_DEPTH; ++i)
    assert(g2RxPacketEnqueue(gR,background.data(),background.size()));
  assert(g2RxPacketEnqueue(gR,pause.data(),pause.size()));
  drainRx(); assert(gConversate.phase == CS::Phase::Paused); stopRun();
  // Old-generation controls cannot tear down the replacement connection.
  startRun(); assert(g2RxPacketEnqueue(gR,close.data(),close.size()));
  ++gR.connectionGeneration; drainRx(); assert(gConversate.phase == CS::Phase::Running);
  g2ConversateTick(); assert(!gConversate.active()); --gR.connectionGeneration;
  // Real owner hooks: native PCM shares the lease, pauses only on explicit
  // native PAUSE, ends cleanly on exit, and fail-closes on transport/loss.
  nativeArmed = true;
  startRun(); assert(nativeCapture && gConversate.audioExchange);
  liveBeat(); assert(gConversate.phase == CS::Phase::Running);
  rx(161,20,9,{8,3}); assert(nativePaused);
  rx(161,21,9,{8,4}); assert(!nativePaused);
  stopRun(); assert(nativeFinishedClean && !nativeCapture);
  nativeCapture = true; // emulate a PCM tail outliving its native CLOSE ACK
  rx(2,1,4); assert(gConversateDeferredPrep && !gConversate.active());
  g2ConversateTick(); assert(gConversateDeferredPrep && !gConversate.active());
  nativeCapture = false;
  g2ConversateTick(); assert(gConversate.phase == CS::Phase::Armed);
  rx(4,3,6,{8,1}); assert(nativeCapture);
  gConversate.duplicates = 1; stopRun(); assert(!nativeFinishedClean);
  startRun(); nativeFailed = true; g2ConversateTick();
  assert(gConversate.reason == CS::Stop::HostAudio && !nativeFinishedClean);
  finishClose(); nativeFailed = false;
  startRun(); gConversate.lostPackets = 1; g2ConversateTick();
  assert(gConversate.reason == CS::Stop::HostAudio); finishClose();
  nativeFailed = true;
  rx(2,1,4); rx(4,3,6,{8,1});
  assert(gConversate.phase == CS::Phase::Closing && gConversate.reason == CS::Stop::HostAudio);
  finishClose(); nativeFailed = nativeArmed = false;
  g2ConversateStop(CS::Stop::Shutdown);
}
