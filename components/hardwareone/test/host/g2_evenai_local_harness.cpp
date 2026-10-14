// Test native presentation and cancellation using the actual shipping adapter.
// Only platform, speech/chat services and BLE transmission are controlled here.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "System_LLMUtf8.h"
#define EXT_RAM_BSS_ATTR
#define DEBUG_G2F(...) ((void)0)
using TransportSessionEpoch = uint32_t;
enum CommandSource { SOURCE_G2_GLASSES };
uint32_t clockMs = 1, liveEpoch = 7, uartEpoch = 11;
uint32_t millis() { return clockMs; }
void portENTER_CRITICAL(int*) {}
void portEXIT_CRITICAL(int*) {}
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
// INSERT_PROTOCOL
#include "g2_evenai_wire_fixture.h"
int gEvenAiSessionMux, gTopologyMux;
bool transportSessionEpochIsLive(CommandSource, uint32_t epoch) { return epoch && epoch == liveEpoch; }
uint32_t uartLinkSessionEpoch() { return uartEpoch; }
struct G2Temple {
  char side = 'L';
  uint32_t connectionGeneration = 8;
  uint32_t sessionReadyGeneration = connectionGeneration;
  bool connected = true, pluginDead = false, writeChar = true;
} gL, gR{'R', 9};
// INSERT_READY
// INSERT_SESSION
G2EvenAiSessionState gEvenAiSession{};
G2EvenAiSessionState g2EvenAiSessionSnapshot() { return gEvenAiSession; }
enum class LlmBackendKind { None, Onboard, Cm5, Http };
constexpr size_t LLM_MODEL_ID_LEN = 48;
struct LlmModelDesc { char id[LLM_MODEL_ID_LEN] = {}; LlmBackendKind backend = LlmBackendKind::Http; };
LlmBackendKind backend = LlmBackendKind::Http;
LlmBackendKind llmBackendActiveKind() { return backend; }
// INSERT_GUARD
// INSERT_BOUND_ARM
bool g2EvenAiTerminate(uint64_t id, const char*) {
  if (id != gEvenAiSession.exchangeId) return false;
  gEvenAiSession.active = false;
  return true;
}
void g2EvenAiMarkActivity(uint64_t id) {
  if (gEvenAiSession.active && gEvenAiSession.exchangeId == id) gEvenAiSession.lastActMs = clockMs;
}
void g2EvenAiSetReplyStreaming(uint64_t, bool on) { gEvenAiSession.replyStreaming = on; }
constexpr int BLE_PEER_G2_GLASSES = 3;
struct BlePeerOwnerSession { uint32_t transportEpoch = 0; std::string user; bool live() { return transportEpoch != 0; } };
bool blePeerOwnerSessionSnapshot(int, BlePeerOwnerSession& p) { p.transportEpoch = liveEpoch; return liveEpoch; }
void secureClearString(std::string& text) { text.clear(); }
void g2ControlWake() {}
bool txOK = true, dismissOnAsk = false;
std::vector<std::string> asks, replies;
std::vector<bool> replyEnds;
unsigned enters = 0, analyses = 0, ends = 0, heartbeats = 0;
bool maySend(uint64_t id) {
  G2Temple* t = g2EvenAiBoundArm(id);
  return txOK && t && g2EvenAiGuardMatches(id, *t);
}
bool g2EvenAiSendCtrl(uint64_t id, uint32_t status) {
  if (!maySend(id)) return false;
  if (status == G2_AI_STATUS_ENTER) ++enters;
  return true;
}
bool g2EvenAiSendAsk(uint64_t id, const char* text) {
  if (!maySend(id)) return false;
  checkNativeWire(text, false, false);
  asks.emplace_back(text);
  if (dismissOnAsk) gEvenAiSession.active = false;
  return true;
}
bool g2EvenAiSendReplyRaw(uint64_t id, const char* text, bool end, bool analyse) {
  if (!maySend(id)) return false;
  assert(text && *text);  // Local adapter must never emit an empty finalizer.
  checkNativeWire(text, true, end);
  replies.emplace_back(text);
  replyEnds.push_back(end);
  ends += end; analyses += analyse;
  return true;
}
bool g2EvenAiSendReply(uint64_t id, const char* text) { return g2EvenAiSendReplyRaw(id, text, true, true); }
bool g2EvenAiSendHeartbeat(uint64_t id) {
  if (!maySend(id)) return false;
  ++heartbeats; return true;
}
// UART admission is not part of a local request. The actual owner tick must
// bypass it even while the model is slow; keep those external services inert.
enum class G2EvenAiHostGate { Ready, SessionChanged, ServiceBusy };
enum class Cm5PresenceMode { Ready, Busy };
struct G2EvenAiHostObservation {
  uint32_t activeSessionEpoch = 0;
  G2EvenAiHostGate cause = G2EvenAiHostGate::Ready;
  Cm5PresenceMode cm5Mode = Cm5PresenceMode::Ready;
  bool cm5CommandInFlight = false;
};
G2EvenAiHostObservation g2EvenAiObserveHost(uint32_t) { assert(false); return {}; }
const char* g2EvenAiHostGateReason(G2EvenAiHostGate) { return nullptr; }
void g2EvenAiLogHostFailure(uint64_t, const char*, const G2EvenAiHostObservation&) {}
bool g2EvenAiTakeEnterPending(uint64_t) { assert(false); return false; }

#if ENABLE_LOCAL_STT && ENABLE_LLM_BACKEND
using STTToken = uint64_t;
using ChatRunToken = uint64_t;
struct STTOwner { CommandSource source = SOURCE_G2_GLASSES; uint32_t epoch = 0; };
enum class STTState { Preparing, Recording, Done, Failed, Cancelled };
struct STTSnapshot { STTState state = STTState::Recording; bool workerActive = true; } speech;
struct STTTextChunk { uint32_t sequence = 0; char text[513] = {}; };
std::vector<std::string> transcripts;
unsigned sttStarts = 0, sttCancels = 0, acks = 0, chatStarts = 0, chatStops = 0;
STTToken nextToken = 20, activeToken = 0;
uint32_t speechEpoch = 0;
bool startSpeech = true, snapshotOK = true, ready = true, busy = false, modelOK = true;
std::string modelName = "http:openclaw", submitted, chatText;
int activeChat = 0, nextChat = 100;
bool chatDone = false;
bool llmBackendActiveModel(LlmModelDesc* m) {
  std::snprintf(m->id, sizeof(m->id), "%s", modelName.c_str()); m->backend = backend; return modelOK;
}
bool llmBackendIsReady() { return ready; }
bool chatIsGenerating() { return busy; }
bool sttBeginUtteranceG2Native(STTOwner owner, uint32_t gen, STTToken* token, char* err, size_t cap, void(*)()) {
  assert(gen == gL.connectionGeneration && owner.epoch == liveEpoch);
  ++sttStarts;
  if (!startSpeech) { snprintf(err, cap, "Microphone is busy"); return false; }
  *token = activeToken = ++nextToken; speechEpoch = owner.epoch; return true;
}
bool sttCancel(STTOwner owner, STTToken token) {
  if (token == activeToken && owner.epoch == speechEpoch) { ++sttCancels; return true; }
  return false;
}
bool sttSnapshot(STTOwner owner, STTToken token, STTSnapshot* out) {
  *out = speech; return snapshotOK && token == activeToken && owner.epoch == speechEpoch;
}
bool sttReadChunk(STTOwner, STTToken, STTTextChunk* out) {
  if (transcripts.empty()) return false;
  out->sequence = acks + 1;
  std::snprintf(out->text, sizeof(out->text), "%s", transcripts.front().c_str()); return true;
}
bool sttAcknowledgeChunk(STTOwner, STTToken, uint32_t seq) {
  assert(seq == acks + 1); ++acks; transcripts.erase(transcripts.begin()); return true;
}
int chatBeginTurn(const char* text, const void*, ChatRunToken* run) {
  if (!ready || busy) return 0;
  ++chatStarts; submitted = text; activeChat = ++nextChat; *run = activeChat; return activeChat;
}
bool chatStopRun(ChatRunToken sid) {
  if (sid != static_cast<ChatRunToken>(activeChat) || chatDone) return false;
  ++chatStops; return true;
}
int chatReadRun(ChatRunToken sid, int offset, char* text, int size, bool* done) {
  *done = chatDone;
  if (sid != static_cast<ChatRunToken>(activeChat)) return -1;
  const std::string piece = chatText.substr(std::min<size_t>(offset, chatText.size()), size - 1);
  std::memcpy(text, piece.c_str(), piece.size() + 1); return piece.size();
}
#endif
// INSERT_ADAPTER
// INSERT_TICK

#if ENABLE_LOCAL_STT && ENABLE_LLM_BACKEND
void reset() {
  g2EvenAiLocalCancel();
  gEvenAiSession = {}; liveEpoch = 7; clockMs = 1;
  gL = G2Temple{}; gR = G2Temple{'R', 9};
  speech = {}; transcripts.clear(); asks.clear(); replies.clear(); replyEnds.clear();
  sttStarts = sttCancels = acks = chatStarts = chatStops = enters = analyses = ends = 0;
  heartbeats = 0;
  txOK = startSpeech = snapshotOK = ready = modelOK = true; busy = dismissOnAsk = chatDone = false;
  modelName = "http:openclaw"; backend = LlmBackendKind::Http;
  submitted.clear(); chatText.clear(); activeChat = 0;
}
void wake(uint64_t id = 1) {
  gEvenAiSession = {};
  gEvenAiSession.active = gEvenAiSession.enterPending = gEvenAiSession.local = true;
  gEvenAiSession.exchangeId = id; gEvenAiSession.armSide = 'R';
  gEvenAiSession.armGeneration = gR.connectionGeneration;
  g2EvenAiLocalTick();
}
void recognized(std::vector<std::string> text = {"hello Mingus"}) {
  transcripts = text; speech.workerActive = false; speech.state = STTState::Done;
  for (size_t i = 0; i <= text.size(); ++i) g2EvenAiLocalTick();
}
void success() {
  reset(); wake(); assert(enters == 1 && sttStarts == 1);
  recognized({"hello", "Mingus"});
  assert(submitted == "hello Mingus" && asks.back() == submitted && chatStarts == 1 && acks == 2);
  chatText = "Hi "; g2EvenAiLocalTick();
  chatText += "\xe2"; g2EvenAiLocalTick(); // incomplete UTF-8 waits, no duplicate bytes
  assert(replies.empty() && std::string(gEvenAiLocal.pendingReply) == "Hi ");
  chatText += "\x98\x83"; chatDone = true; g2EvenAiLocalTick(); g2EvenAiLocalTick();
  assert(replies.size() == 2 && replies[0] == "Hi " && replies[1] == "\xe2\x98\x83");
  assert(!replyEnds[0] && replyEnds[1]);
  assert(analyses == 1 && ends == 1 && gEvenAiLocal.done && !gEvenAiSession.replyStreaming);
  g2EvenAiLocalTick(); assert(chatStarts == 1 && replies.size() == 2);
  // Reply split across the native wire limit, including a multibyte boundary.
  reset(); wake(); recognized(); chatText = std::string(211, 'x') + "\xe2\x98\x83" + std::string(300, 'y');
  chatDone = true; for (int i = 0; i < 5; ++i) g2EvenAiLocalTick();
  std::string joined; for (const auto& r : replies) { assert(r.size() <= 212); joined += r; }
  assert(joined == chatText && analyses == 1 && ends == 1);
  reset(); wake(); recognized({std::string(211, 'x') + "\xe2\x98\x83"});
  assert(asks.back() == std::string(211, 'x') && submitted.size() == 214);
}
void slowModelHeartbeat() {
  reset(); wake();
  // The real session tick keeps sending native heartbeats during a cold STT
  // preparation period, and during a long model wait with no reply bytes.
  for (int i = 0; i < 20; ++i) {
    clockMs += 1000; g2EvenAiSessionTick();
    assert(gEvenAiSession.active && gEvenAiSession.lastActMs == clockMs);
  }
  recognized();
  for (int i = 0; i < 120; ++i) {
    clockMs += 1000; g2EvenAiSessionTick();
    assert(gEvenAiSession.active && gEvenAiSession.lastActMs == clockMs);
  }
  assert(heartbeats == 46 && chatStarts == 1 && replies.empty());
  chatText = "Still here."; chatDone = true;
  g2EvenAiSessionTick(); g2EvenAiSessionTick();
  assert(gEvenAiLocal.done && ends == 1);
  clockMs += 59000; g2EvenAiSessionTick(); assert(gEvenAiSession.active);
  clockMs += 1001; g2EvenAiSessionTick(); assert(!gEvenAiSession.active);
}
void cancellation() {
  reset(); wake(); recognized(); gEvenAiSession.active = false;
  chatText = "late answer"; g2EvenAiLocalTick();
  assert(replies.empty() && chatStops == 1 && !gEvenAiLocal.exchange);
  // A new wake owns a different speech token and cannot consume its predecessor.
  reset(); wake(); wake(2); assert(sttCancels == 1 && sttStarts == 2 && gEvenAiLocal.exchange == 2);
  reset(); wake(); liveEpoch = 8; g2EvenAiLocalTick();
  assert(!gEvenAiSession.active && chatStarts == 0 && replies.empty());
  reset(); wake(); ++gL.connectionGeneration; g2EvenAiLocalTick();
  assert(!gEvenAiSession.active && sttCancels == 1);
  reset(); wake(); dismissOnAsk = true; recognized(); assert(chatStarts == 0);
  reset(); wake(); recognized(); ++activeChat; g2EvenAiLocalTick();
  assert(chatStops == 0 && gEvenAiLocal.done); // another surface's turn is untouched
  reset(); wake(); recognized(); g2EvenAiLocalCancel(); assert(chatStops == 1 && !gEvenAiLocal.exchange);
}
void failures() {
  reset(); liveEpoch = 0; wake(); assert(sttStarts == 0 && enters == 0 && !gEvenAiSession.active);
  reset(); ready = false; wake(); assert(sttStarts == 0 && gEvenAiLocal.done);
  reset(); startSpeech = false; wake(); assert(replies.back() == "Microphone is busy");
  reset(); wake(); speech.state = STTState::Failed; g2EvenAiLocalTick(); assert(chatStarts == 0 && gEvenAiLocal.done);
  reset(); wake(); transcripts = {"partial question"}; g2EvenAiLocalTick();
  speech.state = STTState::Failed; g2EvenAiLocalTick(); assert(chatStarts == 0 && gEvenAiLocal.done);
  reset(); wake(); recognized({}); assert(chatStarts == 0 && gEvenAiLocal.done);
  reset(); wake(); modelName = "http:different"; recognized(); assert(chatStarts == 0);
  reset(); wake(); recognized({std::string(512, 'a'), std::string(512, 'b')}); assert(chatStarts == 0 && gEvenAiLocal.done);
  reset(); wake(); recognized(); chatDone = true; g2EvenAiLocalTick(); assert(replies.back().find("No response") == 0);
  reset(); wake(); clockMs += 180001; g2EvenAiLocalTick(); assert(sttCancels == 1 && gEvenAiLocal.done);
  reset(); wake(); txOK = false; recognized(); assert(!gEvenAiSession.active && chatStarts == 0);
  reset(); backend = LlmBackendKind::Cm5; assert(!g2EvenAiUseLocalSpeech());
  wake(); // native routing would leave local=false, so adapter must stand down
  reset(); gEvenAiSession.active = true; gEvenAiSession.local = false; g2EvenAiLocalTick(); assert(!sttStarts && !chatStarts);
}

void assertCompleteReply(const std::string& expected) {
  std::string joined;
  assert(!replies.empty() && replies.size() == replyEnds.size());
  for (size_t i = 0; i < replies.size(); ++i) {
    assert(!replies[i].empty() && replies[i].size() <= 212);
    assert(replyEnds[i] == (i + 1 == replies.size()));
    joined += replies[i];
  }
  assert(joined == expected && ends == 1 && analyses == 1 && chatStarts == 1);
  assert(gEvenAiLocal.done && !gEvenAiLocal.pendingReplyLen);
  assert(!gEvenAiSession.replyStreaming);
}

void retainedFinalReply() {
  std::string emoji;
  for (int i = 0; i < 53; ++i) emoji += u8"🥔";
  for (const std::string& text : std::vector<std::string>{
           "P4 native ASCII display.", u8"Here's a potato:\n\n🥔",
           std::string(212, 'a'), emoji,
           std::string(211, 'x') + u8"🥔" + std::string(1500, 'z')}) {
    reset(); wake(); recognized(); chatText = text; chatDone = true;
    for (int i = 0; i < 30 && !gEvenAiLocal.done; ++i) g2EvenAiLocalTick();
    assertCompleteReply(text);
  }

  // Completion can arrive without a new text byte. The retained chunk itself
  // becomes final; there is no blank packet after it.
  reset(); wake(); recognized(); chatText = "Waiting only for completion.";
  g2EvenAiLocalTick();
  assert(replies.empty() && gEvenAiLocal.pendingReplyLen == chatText.size());
  g2EvenAiLocalTick(); assert(replies.empty());
  chatDone = true; g2EvenAiLocalTick(); assertCompleteReply(chatText);

  // Earlier chunks remain progressive while exactly one tail waits for EOF.
  reset(); wake(); recognized(); chatText = std::string(212, 'a');
  g2EvenAiLocalTick(); assert(replies.empty());
  chatText += std::string(212, 'b'); g2EvenAiLocalTick();
  assert(replies.size() == 1 && !replyEnds[0]);
  chatText += std::string(212, 'c'); g2EvenAiLocalTick();
  assert(replies.size() == 2 && !replyEnds[1]);
  chatDone = true; g2EvenAiLocalTick(); assertCompleteReply(chatText);

  // Pending text belongs to the exact owner/run; revocation must wipe it and
  // cannot replay either the question or bytes on a replacement surface.
  for (int edge = 0; edge < 4; ++edge) {
    reset(); wake(); recognized(); chatText = "Private pending answer.";
    g2EvenAiLocalTick(); assert(replies.empty() && gEvenAiLocal.pendingReplyLen);
    if (edge == 0) ++liveEpoch;
    if (edge == 1) ++gR.connectionGeneration;
    if (edge == 2) gEvenAiSession.active = false;
    if (edge == 3) ++activeChat;
    g2EvenAiLocalTick();
    assert(gEvenAiLocal.pendingReplyLen == 0 && chatStarts == 1);
    for (const auto& reply : replies) assert(reply != "Private pending answer.");
    if (edge != 3) assert(replies.empty());
    else assert(chatStops == 0); // Never stop another run.
  }

  for (const std::string& invalid : {std::string("\xf0\x9f"), std::string("valid prefix\xf0\x9f")}) {
    reset(); wake(); recognized(); chatText = invalid; chatDone = true;
    for (int i = 0; i < 3 && !gEvenAiLocal.done; ++i) g2EvenAiLocalTick();
    assert(gEvenAiLocal.done && !gEvenAiLocal.pendingReplyLen);
    assert(replies.size() == 1 && replies[0].find("AI returned invalid text") == 0);
    assert(ends == 1 && chatStarts == 1);
  }

  // A failed final display write is terminal and never submits a second turn.
  reset(); wake(); recognized(); chatText = "Pending final.";
  g2EvenAiLocalTick(); txOK = false; chatDone = true; g2EvenAiLocalTick();
  assert(!gEvenAiSession.active && !gEvenAiLocal.pendingReplyLen);
  assert(replies.empty() && chatStarts == 1);
}

void rendererSilenceDoesNotDisableNativeSpeech() {
  reset();
  // The EvenCore renderer can stop answering while the base/native protocol
  // and authenticated transport remain ready. Native voice must still enter.
  gL.pluginDead = gR.pluginDead = true;
  assert(g2TempleReadyAtGeneration(gL, gL.connectionGeneration));
  assert(g2TempleReadyAtGeneration(gR, gR.connectionGeneration));
  wake();
  assert(enters == 1 && sttStarts == 1 && gEvenAiSession.active);
  recognized();
  assert(chatStarts == 1 && submitted == "hello Mingus");
  chatText = "Native reply with renderer silent."; chatDone = true;
  g2EvenAiLocalTick(); g2EvenAiLocalTick();
  assert(ends == 1 && replies.front() == chatText);

  // Renderer independence must not weaken pairing, readiness or generation.
  reset(); gL.pluginDead = gR.pluginDead = true; liveEpoch = 0; wake();
  assert(!enters && !sttStarts && !gEvenAiSession.active);
  reset(); gL.pluginDead = gR.pluginDead = true; gL.sessionReadyGeneration = 0; wake();
  assert(!enters && !sttStarts && !gEvenAiSession.active);
  reset(); gL.pluginDead = gR.pluginDead = true; wake();
  ++gR.connectionGeneration; g2EvenAiLocalTick();
  assert(!gEvenAiSession.active && sttCancels == 1 && !chatStarts);
  reset(); gL.pluginDead = gR.pluginDead = true; wake();
  ++liveEpoch; g2EvenAiLocalTick();
  assert(!gEvenAiSession.active && sttCancels == 1 && !chatStarts);
}
#endif

int main() {
#if ENABLE_LOCAL_STT && ENABLE_LLM_BACKEND
  success(); slowModelHeartbeat(); cancellation(); failures();
  retainedFinalReply();
  rendererSilenceDoesNotDisableNativeSpeech(); reset();
#else
  assert(!g2EvenAiUseLocalSpeech()); g2EvenAiLocalTick(); g2EvenAiLocalCancel();
#endif
  // Packet guard preserves the UART requirement, and local packets require
  // their exact paired-user epoch plus the original temple generation.
  gEvenAiSession = {}; gEvenAiSession.active = true; gEvenAiSession.exchangeId = 9;
  gEvenAiSession.armSide = 'L'; gEvenAiSession.armGeneration = gL.connectionGeneration;
  gEvenAiSession.uartSessionEpoch = 11;
  gL.pluginDead = true;  // Native CM5 traffic also survives renderer silence.
  assert(g2EvenAiGuardMatches(9, gL));
  assert(g2EvenAiBoundArm(9) == &gL);
  // Zero means unbound, never an "any generation" native-session wildcard.
  gEvenAiSession.armGeneration = 0;
  assert(!g2EvenAiGuardMatches(9, gL) && !g2EvenAiBoundArm(9));
  gEvenAiSession.armGeneration = gL.connectionGeneration;
  uartEpoch = 12; assert(!g2EvenAiGuardMatches(9, gL));
  gEvenAiSession.local = true; gEvenAiSession.ownerEpoch = liveEpoch;
  assert(g2EvenAiGuardMatches(9, gL));
  gEvenAiSession.armGeneration = 0;
  assert(!g2EvenAiGuardMatches(9, gL) && !g2EvenAiBoundArm(9));
  gEvenAiSession.armGeneration = gL.connectionGeneration;
  ++liveEpoch; assert(!g2EvenAiGuardMatches(9, gL));
  puts("Native EvenAI adapter: flow, framing, cancellation and authority passed");
}
