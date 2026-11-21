// Synthetic hardware-boundary fixtures for the actual owner + wire codec.
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include <stdint.h>
#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
static void portENTER_CRITICAL(int*) {}
static void portEXIT_CRITICAL(int*) {}
static uint32_t nowMs = 1000;
uint32_t millis() { return nowMs; }
static std::function<void()> onDelay;   // delivers replies scheduled for later (Reply::delayMs)
static void vTaskDelay(unsigned n) { nowMs += n; if (onDelay) onDelay(); }
#define pdMS_TO_TICKS(n) (n)
#define DEBUG_G2F(...) ((void)0)
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)0)
// INSERT_PROTOCOL
class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& s): std::string(s) {}
  void toLowerCase() { for (auto& c : *this) c = char(std::tolower((unsigned char)c)); }
  void trim() {
    const char* blanks = " \t\r\n";
    erase(0, find_first_not_of(blanks));
    erase(find_last_not_of(blanks) + 1);
  }
};
class CommandArgs {
  std::vector<String> args;
 public:
  explicit CommandArgs(const String& value) {
    std::istringstream in(value); std::string word;
    while (in >> word) args.emplace_back(word);
  }
  size_t count() const { return args.size(); }
  const String& arg(size_t i) const { return args.at(i); }
  int argInt(size_t i, int fallback) const { return i < args.size() ? std::stoi(args[i]) : fallback; }
};
static std::string broadcast;
static void capture(const char* format, ...) {
  char text[256]; va_list ap; va_start(ap, format);
  vsnprintf(text, sizeof(text), format, ap); va_end(ap);
  broadcast += text; broadcast += '\n';
}
#define BROADCAST_PRINTF(...) capture(__VA_ARGS__)
#include "G2_QuickListCore.h"
using SemaphoreHandle_t = int*;
static constexpr int pdTRUE = 1;
static std::function<void()> onGate, onStats;
static bool bleCentralTxTake(unsigned) { if (onGate) onGate(); return true; }
// Each settings read send (sendEnvelopeAtGeneration): false = the send fails
// (as when the central TX gate or the write mutex times out).
static std::function<bool()> onReadSend;
static void bleCentralTxGive() {}
static int xSemaphoreTake(SemaphoreHandle_t, unsigned) { return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t) {}
struct FakeWrite { bool writeValue(uint8_t*, size_t, bool); };
struct FakeClient { uint16_t getMTU() { return 244; } };
struct G2Temple {
  char side;
  bool connected = true, pluginDead = false;
  uint32_t connectionGeneration = 7, sessionReadyGeneration = 7;
  FakeWrite* writeChar = nullptr;
  FakeClient* client = nullptr;
  uint16_t mtu = 244;
  unsigned packetsSent = 0;
  SemaphoreHandle_t writeMutex = nullptr;
  char firmwareVersion[32] = {};
};
static FakeWrite rightWrite, leftWrite;
static int fakeMutex;
static G2Temple gL{'L'}, gR{'R'};
static int gTopologyMux, gRxPacketMux;
static char gFwVersion[32] = {};
// INSERT_TYPES_STATE
static bool g2EvenAiGuardMatches(uint64_t, const G2Temple&) { return true; }
static bool g2CreateCleanupWireCurrent(uint32_t, const G2Temple&) { return true; }
static bool g2CreateWireCurrent(uint32_t, const G2Temple&) { return true; }
static bool g2LensTxFenceCurrent(const G2Temple&, uint32_t, uint32_t) { return true; }
static uint32_t presentationEpoch = 1;
static uint32_t g2PresentationEpochCurrent() { return presentationEpoch; }
static uint32_t nativeTerminalEpoch = 73;
static bool g2NativeTerminalGuardMatches(uint32_t epoch, const G2Temple& temple) {
  return epoch && epoch == nativeTerminalEpoch && temple.side == 'R';
}
// The sender takes an optional native-notes context; no case here passes one.
struct G2NoteContext;
static bool g2NativeNotesContextLive(const G2NoteContext&) { return true; }
static bool g2LinkIsSlow(const G2Temple*) { return false; }
static void statsBoundary(uint8_t sid, uint8_t flag, size_t len) { g2statsRecordTx(sid, flag, len); if (onStats) onStats(); }
#define g2statsRecordTx statsBoundary
static void g2RingRecord(char, char, const uint8_t*, size_t) {}
static constexpr unsigned G2_CENTRAL_TX_ENVELOPE_MS = 1, G2_SEND_ENVELOPE_MUTEX_MS = 1;
static constexpr unsigned G2_WRITE_RC_RETRY_COOLDOWN_RING_MS = 1, G2_WRITE_RC_RETRY_COOLDOWN_MS = 1;
#define G2_SETTINGS_FRESH_MS 30000
static unsigned controlEchoes, controlAcks;
static void g2ControlNoteEcho(const G2SettingsEcho&, uint32_t) { ++controlEchoes; }
static void g2ControlNoteAck(uint32_t, uint8_t, uint8_t, uint32_t, uint32_t) { ++controlAcks; }
static uint8_t allocSeq() { static uint8_t seq; return ++seq; }
static bool sendEnvelopeAtGeneration(G2Temple&, const uint8_t*, size_t, uint32_t);
// INSERT_OWNER
static bool sendEnvelopeAtGeneration(G2Temple& t, const uint8_t* data, size_t len, uint32_t gen) {
  if (onReadSend && !onReadSend()) return false;
  return gen && g2TempleReadyAtGeneration(t, gen) &&
      sendEnvelopeNoMutex(t, data, len, 0, nullptr, 0, 0, gen);
}
// G2_QuickList boundary shims: no voice session, the FULL_UPDATE send is counted.
static bool g2NativeVoiceActive() { return false; }
static bool quickNoteActive() { return false; }
static unsigned quickListSends;
static bool sendPbFragmented(G2Temple& arm, uint8_t, uint8_t sid, uint8_t flag, const uint8_t*, size_t,
                             uint32_t generation) {
  if (arm.side != 'R' || sid != 0x0C || flag != G2_FLAG_REQUEST || !g2TempleReadyAtGeneration(arm, generation))
    return false;
  ++quickListSends;
  return true;
}
static void g2ControlWake() {}
#define G2_ASSERT_NOT_ACK_OWNER_TASK(who) ((void)(who))
// INSERT_QUICKLIST

// Replies describe independent wire fixtures; they do not emulate admission or
// confirmation logic, which is supplied by the production owner above.
struct Reply {
  bool send = true, fullVersions = true, asyncAfter = false;
  int wear = 0; // -1 = absent nanopb scalar
  int silent = -1; // BASIC f14; -1 = absent (silent off on a full 2.3.2.14 reply)
  int brightness = -1, autoBrightness = -1; // -1 = absent
  int headUp = -1; // BASIC f7; -1 = absent (head-up off on a full 2.3.2.14 reply, to confirm on the device)
  uint8_t flag = G2_FLAG_RESPONSE;
  int magicDelta = 0;
  unsigned delayMs = 0;   // > 0: the reply arrives this long after the read is sent
  char side = 'R';
  int generationDelta = 0;
  const char* version = "2.3.2.14";
};
static std::vector<Reply> replies;
static unsigned reads, writes, physicalWrites;
static bool sendAck, wrongAckValue, emptyWearAck, wrongAckInner, wrongAckLeaf, rolloverAfterWrite, rolloverAtReadback;
static uint8_t ackFlag;
static char ackSide;
static int ackGenerationDelta, ackMagicDelta;
static std::vector<uint8_t> sentMagic;
// Every setting write as sent: (inner, leaf, value) and the payload bytes.
struct WriteRec { uint8_t inner, leaf; uint32_t value; std::vector<uint8_t> payload; };
static std::vector<WriteRec> writeLog;
// When set, a write is ACKed only if this returns true (sendAck still applies).
static std::function<bool(uint8_t, uint8_t, uint32_t)> ackIf;
// Simulated send times: every BASIC read, and the last setting write.
static std::vector<uint32_t> readAtMs;
static uint32_t writeAtMs;
static std::function<void()> afterPhysical;
struct Scheduled { uint32_t atMs; uint8_t magic; Reply reply; };
static std::vector<Scheduled> scheduled;

static void receive(const uint8_t* payload, size_t length, uint8_t flag,
                    char side = 'R', int generationDelta = 0) {
  G2EnvelopeView frame{}; frame.sid = G2_SID_SETTINGS; frame.flag = flag;
  frame.payload = payload; frame.payloadLen = length;
  auto& temple = side == 'R' ? gR : gL;
  const uint32_t gen = uint32_t(int(temple.connectionGeneration) + generationDelta);
  g2SettingsReceive(temple, frame, gen);
  // The remaining production RX dispatcher publishes version-bearing packets.
  G2SettingVersions versions{};
  if (gen == temple.connectionGeneration && g2ParseSettingVersions(payload, length, &versions)) {
    if (versions.hasLeft) snprintf(gL.firmwareVersion, sizeof(gL.firmwareVersion), "%s", versions.left);
    if (versions.hasRight) snprintf(gR.firmwareVersion, sizeof(gR.firmwareVersion), "%s", versions.right);
  }
}
static void replyRead(uint8_t magic, const Reply& reply) {
  if (!reply.send) return;
  uint8_t body[192]; size_t pos = 0, inner;
  assert(g2PbWriteUint32(body, sizeof(body), &pos, 1, 2));
  assert(g2PbWriteUint32(body, sizeof(body), &pos, 2, uint8_t(magic + reply.magicDelta)));
  assert(g2PbBeginNested(body, sizeof(body), &pos, 4, &inner));
  assert(g2PbWriteUint32(body, sizeof(body), &pos, 1, 1));
  if (reply.brightness >= 0) assert(g2PbWriteUint32(body, sizeof(body), &pos, 2, reply.brightness));
  if (reply.fullVersions) {
    assert(g2PbWriteString(body, sizeof(body), &pos, 5, reply.version));
    assert(g2PbWriteString(body, sizeof(body), &pos, 6, reply.version));
  }
  if (reply.headUp >= 0) assert(g2PbWriteUint32(body, sizeof(body), &pos, 7, reply.headUp));
  if (reply.wear >= 0) assert(g2PbWriteUint32(body, sizeof(body), &pos, 10, reply.wear));
  if (reply.silent >= 0) assert(g2PbWriteUint32(body, sizeof(body), &pos, 14, reply.silent));
  if (reply.autoBrightness >= 0)
    assert(g2PbWriteUint32(body, sizeof(body), &pos, 18, reply.autoBrightness));
  assert(g2PbEndNested(body, sizeof(body), &pos, inner));
  receive(body, pos, reply.flag, reply.side, reply.generationDelta);
  if (reply.asyncAfter) {
    Reply push; push.wear = -1; push.flag = G2_FLAG_NOTIFY;
    replyRead(250, push);
  }
}
bool FakeWrite::writeValue(uint8_t* data, size_t length, bool) {
  ++physicalWrites;
  if (afterPhysical) afterPhysical();
  G2EnvelopeView frame{};
  if (!g2ParseEnvelope(data, length, &frame)) return true; // Raw ATT continuation fixture.
  uint32_t magic = 0, command = 0; G2SettingsEcho echo{};
  if (g2ParseSettingEcho(frame.payload, frame.payloadLen, &echo, &magic, &command) && command == 2) {
    ++reads; sentMagic.push_back(uint8_t(magic)); readAtMs.push_back(nowMs);
    if (rolloverAtReadback && writes) {
      ++gR.connectionGeneration; gR.sessionReadyGeneration = gR.connectionGeneration;
      g2SettingsEchoInvalidateFreshness();
    }
    if (reads <= replies.size()) {
      const Reply& reply = replies[reads - 1];
      if (reply.delayMs) scheduled.push_back({nowMs + reply.delayMs, uint8_t(magic), reply});
      else replyRead(uint8_t(magic), reply);
    }
    return true;
  }
  uint8_t inner = 0, leaf = 0; uint32_t value = 0;
  assert(g2ParseSettingWriteAck(frame.payload, frame.payloadLen, &magic, &inner, &leaf, &value));
  ++writes; sentMagic.push_back(uint8_t(magic)); writeAtMs = nowMs;
  writeLog.push_back({inner, leaf, value, std::vector<uint8_t>(frame.payload, frame.payload + frame.payloadLen)});
  if (rolloverAfterWrite) {
    ++gR.connectionGeneration; gR.sessionReadyGeneration = gR.connectionGeneration;
    g2SettingsEchoInvalidateFreshness();
  }
  if (sendAck && (!ackIf || ackIf(inner, leaf, value))) {
    uint8_t ack[128];
    size_t pos = 0, body, setting;
    assert(g2PbWriteUint32(ack, sizeof(ack), &pos, 1, 1));
    assert(g2PbWriteUint32(ack, sizeof(ack), &pos, 2, uint8_t(magic + ackMagicDelta)));
    assert(g2PbBeginNested(ack, sizeof(ack), &pos, 3, &body));
    assert(g2PbBeginNested(ack, sizeof(ack), &pos, wrongAckInner ? 2 : inner, &setting));
    if (!emptyWearAck)
      assert(g2PbWriteUint32(ack, sizeof(ack), &pos, wrongAckLeaf ? 2 : leaf,
                            wrongAckValue ? value ^ 1u : value));
    assert(g2PbEndNested(ack, sizeof(ack), &pos, setting));
    assert(g2PbEndNested(ack, sizeof(ack), &pos, body));
    receive(ack, pos, ackFlag, ackSide, ackGenerationDelta);
  }
  return true;
}
static void reset(const char* version = "2.3.2.14") {
  presentationEpoch = 1;
  nativeTerminalEpoch = 73;
  gL = G2Temple{'L'}; gR = G2Temple{'R'};
  gL.writeChar = &leftWrite; gR.writeChar = &rightWrite;
  gL.writeMutex = gR.writeMutex = &fakeMutex;
  snprintf(gL.firmwareVersion, sizeof(gL.firmwareVersion), "%s", version);
  snprintf(gR.firmwareVersion, sizeof(gR.firmwareVersion), "%s", version);
  snprintf(gFwVersion, sizeof(gFwVersion), "%s", version);
  g2SettingsEchoInvalidateFreshness();
  reads = writes = physicalWrites = controlEchoes = controlAcks = 0;
  sendAck = true; wrongAckValue = emptyWearAck = wrongAckInner = wrongAckLeaf = rolloverAfterWrite = rolloverAtReadback = false;
  ackFlag = G2_FLAG_RESPONSE; ackSide = 'R'; ackGenerationDelta = ackMagicDelta = 0;
  replies = {Reply{}, Reply{}};
  sentMagic.clear(); readAtMs.clear(); writeAtMs = 0;
  writeLog.clear(); ackIf = nullptr;
  broadcast.clear(); onGate = onStats = afterPhysical = nullptr;
  onReadSend = nullptr;
  scheduled.clear();
  onDelay = [] {
    std::vector<Scheduled> due;
    for (auto it = scheduled.begin(); it != scheduled.end();) {
      if (int32_t(nowMs - it->atMs) >= 0) { due.push_back(*it); it = scheduled.erase(it); } else ++it;
    }
    for (const auto& d : due) replyRead(d.magic, d.reply);
  };
}
// A reply that never comes (the read's attempt goes unanswered).
static Reply noReply() { Reply r; r.send = false; return r; }
// The right temple reconnects (a new generation): the link a job runs on is gone.
static void rolloverRight() {
  ++gR.connectionGeneration; gR.sessionReadyGeneration = gR.connectionGeneration;
  g2SettingsEchoInvalidateFreshness();
}
// Rolls the right temple over at the first simulated delay `afterMs` or more
// after the setting write (a reconnect while the write waits to read back).
static void rolloverAfterWriteBy(uint32_t afterMs) {
  static uint32_t after;
  static bool done;
  after = afterMs; done = false;
  const auto deliver = onDelay;
  onDelay = [deliver] {
    deliver();
    if (!done && writes && nowMs - writeAtMs >= after) { done = true; rolloverRight(); }
  };
}
static bool has(const std::string& text, const char* fragment) { return text.find(fragment) != std::string::npos; }
static std::string hexOf(const std::vector<uint8_t>& b) {
  std::string out; char two[3];
  for (uint8_t v : b) { snprintf(two, sizeof two, "%02x", v); out += two; }
  return out;
}
// The captured 2.3.2.14 write layout with this write's own magic:
// 08 01 10 <magic varint> 1a 04 <setting body>.
static std::string capturedWrite(uint8_t magic, const char* body) {
  char m[8];
  if (magic < 0x80) snprintf(m, sizeof m, "%02x", magic);
  else snprintf(m, sizeof m, "%02x01", magic);   // magic < 256: two-byte varint
  return std::string("080110") + m + "1a04" + body;
}
static std::string run(const char* command = "weardetect off") { return cmd_g2glasses(String(command)); }
// The executor reads a reply that leads with "Error" as a failed command.
static bool failed(const std::string& text) { return text.rfind("Error", 0) == 0; }

int main() {
  reset(); gR.pluginDead = true;
  assert(has(run(), "confirmed by fresh RIGHT readback and matching ACK"));
  assert(writes == 1 && reads == 2 && sentMagic[0] != sentMagic[1] && sentMagic[1] != sentMagic[2]);

  reset(); replies[0].wear = replies[1].wear = -1; emptyWearAck = true;
  assert(has(run(), "confirmed by fresh RIGHT readback and matching ACK"));
  G2SettingsEcho cached{}; bool full = false;
  assert(g2SettingsEchoSnapshot(&cached, nullptr, 7, &full) && full && !G2_ECHO_HAS(cached, 10));
  assert(has(run("weardetect"), "= off"));
  run("show"); assert(has(broadcast, "weardetect  off"));
  Reply unsolicited; unsolicited.wear = -1; unsolicited.flag = G2_FLAG_NOTIFY;
  replyRead(250, unsolicited);
  assert(g2SettingsEchoSnapshot(&cached, nullptr, 7, &full) && !full);
  assert(has(run("weardetect"), "not reported"));
  // Bare show on that partial snapshot reads again: the correlated full reply
  // turns the omitted wear field into "off" instead of an unknown.
  { const unsigned before = reads;
    replies.resize(reads);   // the reads so far (the single get above resent its read) used theirs
    replies.push_back(Reply{}); replies.back().wear = -1;
    replies.push_back(Reply{}); replies.back().wear = -1;
    broadcast.clear(); run("show");
    assert(reads > before && has(broadcast, "weardetect  off") && !has(broadcast, "weardetect  (")); }
  // When the fresh read gets no answer, the line says the reply left it out.
  reset(); replies[0].wear = -1; replies[0].flag = G2_FLAG_NOTIFY; replies[1].send = false;
  { Reply partial; partial.wear = -1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {}; broadcast.clear(); run("show");
  assert(has(broadcast, "weardetect  (left out of this reply"));

  // Silent mode on the exact 2.3.2.14 pair: written like wear and confirmed by
  // BASIC f14 (present = on; left out of a full correlated reply = off).
  reset(); replies[1].silent = 1;
  assert(run("silent on") == "G2 glasses: silent = 1 confirmed by fresh RIGHT readback and matching ACK");
  assert(writes == 1 && reads == 2);
  assert(has(run("silent"), "= on"));
  broadcast.clear(); run("show"); assert(has(broadcast, "silent      on"));
  // Off: the captured ACK leaves the zero out (empty f6), the reply leaves f14 out.
  reset(); replies[0].silent = 1; emptyWearAck = true;
  assert(run("silent off") == "G2 glasses: silent = 0 confirmed by fresh RIGHT readback and matching ACK");
  assert(writes == 1 && reads == 2);
  assert(has(run("silent"), "= off"));
  broadcast.clear(); run("show"); assert(has(broadcast, "silent      off"));
  // Already on: the glasses send no ACK, the read-back still confirms it.
  reset(); replies[0].silent = replies[1].silent = 1; sendAck = false;
  assert(has(run("silent on"), "confirmed by fresh RIGHT readback (no matching ACK)") && writes == 1);
  // A read-back that disagrees is an error.
  reset(); replies[1].silent = -1;
  { const auto result = run("silent on"); assert(failed(result) && has(result, "fresh readback is 0 despite matching ACK")); }
  // No full first reply, no write.
  for (unsigned fault = 0; fault < 3; ++fault) {
    reset();
    if (fault == 0) replies[0].send = false;
    if (fault == 1) replies[0].fullVersions = false;
    if (fault == 2) replies[0].flag = G2_FLAG_NOTIFY;
    assert(has(run("silent on"), "silent write not sent") && writes == 0);
  }
  // A partial snapshot that carries wear but not f14 is re-read by bare show,
  // so silent prints off instead of an unknown.
  reset();
  { Reply partial; partial.wear = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {Reply{}};
  { const unsigned before = reads;
    broadcast.clear(); run("show");
    assert(reads > before && has(broadcast, "silent      off") && !has(broadcast, "silent      (")); }
  // The single-knob read re-reads that partial snapshot the same way.
  reset();
  { Reply partial; partial.wear = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {Reply{}};
  { const unsigned before = reads;
    assert(has(run("silent"), "= off") && reads == before + 1); }
  // Without an answer silent is unknown: the glasses' own updates never carry
  // f14, even while silent is on, so the "omit zero/off" wording would mislead.
  reset();
  { Reply partial; partial.wear = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {}; broadcast.clear(); run("show");
  assert(has(broadcast, "silent      (unknown: only a full settings read reports it"));
  { const auto result = run("silent");
    assert(has(result, "unknown: the glasses' own updates never carry silent mode") &&
           !has(result, "omit zero/off")); }
  // Earlier firmware has no silent read-back: unchanged wording, ack only.
  reset("2.2.4.34"); replies[0].version = replies[1].version = "2.2.4.34";
  broadcast.clear(); run("show");
  assert(has(broadcast, "silent      (not reported by this firmware)"));
  assert(has(run("silent"), "not reported by this firmware"));
  // `g2glasses help` prints each setting's help line, with the captured ranges.
  broadcast.clear();
  assert(has(run("help"), "settings listed"));
  assert(has(broadcast, "display height 0-12 (12 = top)") &&
         has(broadcast, "display distance 0 close, 1 medium, 2 far") &&
         has(broadcast, "silent / DND (read back on 2.3.2.14)") &&
         has(broadcast, "auto brightness (read back on 2.3.2.14)"));
  assert(has(run("nosuch"), "show, refresh or help."));
  reset("2.2.4.34"); replies[0].version = replies[1].version = "2.2.4.34"; sendAck = false;
  assert(has(run("silent on"), "sent without matching ACK or fresh readback; unconfirmed") && writes == 1);

  // Auto brightness on the exact 2.3.2.14 pair: f1 leaf 1, confirmed by BASIC
  // f18 (present = on; left out of a full correlated reply = off, which is
  // inferred from f10/f14 and still to confirm on the device test).
  reset(); replies[1].autoBrightness = 1;
  assert(run("autobright on") == "G2 glasses: autobright = 1 confirmed by fresh RIGHT readback and matching ACK");
  assert(writes == 1 && reads == 2);
  assert(has(run("autobright"), "= on"));
  broadcast.clear(); run("show"); assert(has(broadcast, "autobright  on"));
  // Off: the captured ACK keeps the explicit zero (08 00); the reply leaves f18 out.
  reset(); replies[0].autoBrightness = 1;
  assert(run("autobright off") == "G2 glasses: autobright = 0 confirmed by fresh RIGHT readback and matching ACK");
  assert(writes == 1 && reads == 2);
  assert(has(run("autobright"), "= off"));
  broadcast.clear(); run("show"); assert(has(broadcast, "autobright  off"));
  // An empty brightness body is not the captured ACK and never matches (it
  // could be the level or auto); the read-back alone confirms the write.
  reset(); replies[0].autoBrightness = 1; emptyWearAck = true;
  assert(run("autobright off") == "G2 glasses: autobright = 0 confirmed by fresh RIGHT readback (no matching ACK)");
  // Already on: no ACK, the read-back confirms it.
  reset(); replies[0].autoBrightness = replies[1].autoBrightness = 1; sendAck = false;
  assert(has(run("autobright on"), "confirmed by fresh RIGHT readback (no matching ACK)") && writes == 1);
  // A read-back that disagrees is an error.
  reset();
  { const auto result = run("autobright on"); assert(failed(result) && has(result, "fresh readback is 0 despite matching ACK")); }
  // No full first reply, no write.
  for (unsigned fault = 0; fault < 3; ++fault) {
    reset();
    if (fault == 0) replies[0].send = false;
    if (fault == 1) replies[0].fullVersions = false;
    if (fault == 2) replies[0].flag = G2_FLAG_NOTIFY;
    assert(has(run("autobright on"), "autobright write not sent") && writes == 0);
  }
  // A glasses push without f18 says nothing: bare show and the single get read
  // again, and the full reply's missing f18 then reads as off.
  reset();
  { Reply partial; partial.wear = 1; partial.silent = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {Reply{}};
  { const unsigned before = reads;
    broadcast.clear(); run("show");
    assert(reads == before + 1 && has(broadcast, "autobright  off") && !has(broadcast, "autobright  (")); }
  reset();
  { Reply partial; partial.wear = 1; partial.silent = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {Reply{}};
  { const unsigned before = reads;
    assert(has(run("autobright"), "= off") && reads == before + 1); }
  // Without an answer it stays unreported, never "= off".
  reset();
  { Reply partial; partial.wear = 1; partial.silent = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {};
  { const auto result = run("autobright"); assert(has(result, "not reported") && !has(result, "= off")); }
  // A push that carries f18 = 1 is on without another read.
  reset();
  { Reply partial; partial.wear = 1; partial.silent = 1; partial.autoBrightness = 1; partial.flag = G2_FLAG_NOTIFY;
    replyRead(250, partial); }
  replies = {};
  { const unsigned before = reads; assert(has(run("autobright"), "= on") && reads == before); }
  // The brightness level on 2.3.2.14 (captured 2026-11-21: f3{f1{f2: level}},
  // the ACK echoes it): written after a full pre-read, read back as BASIC f2.
  reset(); replies[1].brightness = 50;
  assert(run("brightness 50") == "G2 glasses: brightness = 50 confirmed by fresh RIGHT readback and matching ACK");
  assert(writes == 1 && reads == 2 && writeLog[0].inner == 1 && writeLog[0].leaf == 2 && writeLog[0].value == 50);
  assert(hexOf(writeLog[0].payload) == capturedWrite(sentMagic[1], "0a021032"));
  // With auto brightness on the glasses move the level again by themselves:
  // an ACKed level that reads back as another one is a plain report.
  reset(); replies[0].autoBrightness = replies[1].autoBrightness = 1; replies[1].brightness = 64;
  assert(run("brightness 50") == "G2 glasses: brightness = 50 acked by RIGHT glasses. Fresh readback is 64: "
                                  "auto brightness is on and moves the level by itself.");
  // No full pre-read, no write.
  reset(); replies[0].fullVersions = false;
  assert(has(run("brightness 50"), "brightness write not sent") && writes == 0);
  // 0 and 1 were never sent by the app on 2.3.2.14 (its lowest is 2): refused
  // before any read or write; 2 is admitted.
  for (const char* low : {"brightness 0", "brightness 1"}) {
    reset();
    const unsigned before = reads;
    assert(has(run(low), "must be 2..100") && writes == 0 && reads == before);
  }
  reset(); replies[1].brightness = 2;
  assert(has(run("brightness 2"), "confirmed") && writes == 1 && writeLog[0].value == 2);
  // The glasses keep only even levels (device test 2026-11-21): an odd level
  // reads back one lower, which is the write confirmed, not a failure.
  reset(); replies[1].brightness = 24;
  { const auto result = run("brightness 25");
    assert(!failed(result) && result == "G2 glasses: brightness = 25 stored as 24 (the glasses keep even levels), "
                                         "confirmed by fresh RIGHT readback and matching ACK");
    assert(writes == 1 && reads == 2 && writeLog[0].value == 25); }
  reset(); replies[1].brightness = 74;
  { const auto result = run("brightness 75");
    assert(!failed(result) && has(result, "brightness = 75 stored as 74 (the glasses keep even levels), confirmed"));
    assert(writes == 1 && writeLog[0].value == 75); }
  // Nothing wider: an even level that reads back one lower is still a failure.
  reset(); replies[1].brightness = 49;
  { const auto result = run("brightness 50");
    assert(failed(result) && has(result, "requested 50, fresh readback is 49 despite matching ACK")); }
  // The head-up switch on 2.3.2.14 (captured: f3{f4{f1}}, the ACK keeps the
  // explicit zero), read back as BASIC f7: present = on, left out of a full
  // reply = off (inferred, to confirm on the device).
  reset(); replies[0].headUp = 1;
  assert(run("headup off") == "G2 glasses: headup = 0 confirmed by fresh RIGHT readback and matching ACK");
  assert(writes == 1 && reads == 2 && hexOf(writeLog[0].payload) == capturedWrite(sentMagic[1], "22020800"));
  assert(has(run("headup"), "= off"));
  reset(); replies[1].headUp = 1;
  assert(run("headup on") == "G2 glasses: headup = 1 confirmed by fresh RIGHT readback and matching ACK");
  assert(hexOf(writeLog[0].payload) == capturedWrite(sentMagic[1], "22020801"));
  reset(); replies[0].fullVersions = false;
  assert(has(run("headup on"), "headup write not sent") && writes == 0);
  // A glasses push without f7 says nothing: the single get reads again.
  reset();
  { Reply partial; partial.wear = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {Reply{}};
  { const unsigned before = reads;
    assert(has(run("headup"), "= off") && reads == before + 1); }
  // The head-up angle stays refused on 2.3.2.14.
  reset();
  { const auto result = run("headangle 60");
    assert(failed(result) && has(result, "not enabled") && writes == 0 && reads == 0); }

  // `g2glasses refresh` resends the read: one unanswered attempt is not a failure.
  reset(); replies = {noReply(), Reply{}}; replies[1].silent = 1;
  broadcast.clear(); assert(!failed(run("refresh")) && reads == 2 && has(broadcast, "silent      on"));
  reset(); replies = {noReply(), noReply(), noReply()};
  { const auto result = run("refresh"); assert(failed(result) && has(result, "did not answer") && reads == 3); }
  // Bare show on a snapshot from the glasses' own updates: the re-read's first
  // attempt goes unanswered, its resend is answered, and silent shows off.
  reset();
  { Reply partial; partial.wear = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {noReply(), Reply{}};
  { const unsigned before = reads;
    broadcast.clear(); run("show");
    assert(reads == before + 2 && has(broadcast, "silent      off") && !has(broadcast, "silent      (")); }
  // A slow reply (600 ms) to the re-read is waited for.
  reset();
  { Reply partial; partial.wear = 1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  replies = {Reply{}}; replies[0].delayMs = 600; replies[0].silent = 1;
  broadcast.clear(); run("show"); assert(reads == 1 && has(broadcast, "silent      on"));
  // No snapshot at all and the short priming read unanswered: bare show reads
  // again with the long read instead of giving up.
  reset(); replies = {noReply(), Reply{}};
  broadcast.clear();
  { const auto result = run("show"); assert(!failed(result) && reads == 2 && has(broadcast, "silent      off")); }
  // CLI writes: one attempt with the long wait (a 600 ms reply is waited for,
  // an unanswered pre-read is not resent and nothing is written).
  reset(); replies[0].delayMs = 600; replies[1].silent = 1;
  assert(run("silent on") == "G2 glasses: silent = 1 confirmed by fresh RIGHT readback and matching ACK");
  reset(); replies = {noReply(), Reply{}};
  assert(has(run("silent on"), "silent write not sent") && reads == 1 && writes == 0);

  // QuickList boundary: one fresh read gives every row's switch.
  { namespace ql = hw1_g2_quicklist;
    ql::ReadResult rd{};
    reset(); replies[0].wear = 1; replies[0].silent = 1;
    assert(g2QuickListSettingsRead(7, &rd) && rd.readOk && rd.fullProfile);
    assert(rd.sw[0].known && rd.sw[0].on && rd.sw[1].known && rd.sw[1].on && writes == 0);
    reset(); replies[0].wear = -1;   // both left out of a full reply: both off
    assert(g2QuickListSettingsRead(7, &rd) && rd.sw[0].known && !rd.sw[0].on && rd.sw[1].known && !rd.sw[1].on);
    reset(); replies[0].fullVersions = false; replies[0].silent = 1;   // partial: absent = unknown
    assert(g2QuickListSettingsRead(7, &rd) && !rd.fullProfile && rd.sw[0].known && rd.sw[1].known && rd.sw[1].on);
    reset(); replies[0].fullVersions = false; replies[0].wear = -1;
    assert(g2QuickListSettingsRead(7, &rd) && !rd.sw[0].known && !rd.sw[1].known);
    // Unanswered: the captured BASIC read three times, a new magic each, ~1 s wait each.
    reset(); replies = {noReply(), noReply(), noReply(), Reply{}};
    assert(!g2QuickListSettingsRead(7, &rd) && !rd.readOk && reads == 3 && writes == 0);
    assert(sentMagic[0] != sentMagic[1] && sentMagic[1] != sentMagic[2] && sentMagic[0] != sentMagic[2]);
    assert(rd.diag.attempts == 3 && !ql::readAnswered(rd.diag) && rd.diag.totalMs >= 3000 && rd.diag.totalMs < 3100);
    // The first attempt unanswered, the resend answered: real values, no "?".
    reset(); replies = {noReply(), Reply{}}; replies[1].silent = 1;
    assert(g2QuickListSettingsRead(7, &rd) && rd.fullProfile && rd.sw[1].known && rd.sw[1].on && reads == 2);
    assert(rd.diag.attempts == 2 && rd.diag.answeredAttempt == 2 && ql::readNeededResend(rd.diag));
    // A slow reply (400 ms; the wait used to be 250 ms) answers the first attempt.
    reset(); replies[0].delayMs = 400; replies[0].silent = 1;
    assert(g2QuickListSettingsRead(7, &rd) && rd.sw[1].on && reads == 1);
    assert(rd.diag.attempts == 1 && rd.diag.answeredAttempt == 1 && rd.diag.replyMs >= 400 && rd.diag.replyMs < 420);
    // A reply to attempt 1 that arrives during attempt 2's wait answers the
    // read, recorded as attempt 1's (1.3 s after its send).
    reset(); replies = {Reply{}, noReply(), noReply()}; replies[0].delayMs = 1300; replies[0].silent = 1;
    assert(g2QuickListSettingsRead(7, &rd) && rd.sw[1].on && reads == 2);
    assert(rd.diag.attempts == 2 && rd.diag.answeredAttempt == 1 && rd.diag.replyMs >= 1300 && rd.diag.replyMs < 1320);
    // A late reply to an earlier read never answers a newer one: it is counted
    // as another reply during the newer read's wait.
    reset(); replies = {noReply(), noReply(), Reply{}, noReply(), noReply(), noReply()};
    replies[2].delayMs = 2000; replies[2].silent = 1;   // read 1's third attempt, answered after read 1 ended
    assert(!g2QuickListSettingsRead(7, &rd) && reads == 3);
    assert(!g2QuickListSettingsRead(7, &rd) && !rd.readOk && reads == 6);
    assert(rd.diag.otherReplies == 1 && rd.diag.otherFrames == 0);
    // Frames that do not answer: the glasses' own update (no settingInfoType 1
    // reply) and a reply with another magic, each counted.
    reset(); replies = {Reply{}, Reply{}, noReply()};
    replies[0].flag = G2_FLAG_NOTIFY; replies[1].magicDelta = 100;
    assert(!g2QuickListSettingsRead(7, &rd) && reads == 3);
    assert(rd.diag.attempts == 3 && rd.diag.otherFrames == 1 && rd.diag.otherReplies == 1);
    // Attempt 2's send fails (the TX gate held 1.5 s) while attempt 1's slow
    // reply arrives: the read is answered by attempt 1, past the resend window,
    // and no further attempt is sent.
    {
      static unsigned sends;
      sends = 0;
      reset(); replies = {Reply{}, noReply(), Reply{}}; replies[0].delayMs = 1800; replies[0].silent = 1;
      onReadSend = [] {
        if (++sends != 2) return true;
        nowMs += 1500; onDelay();   // the gate wait, then the send fails
        return false;
      };
      assert(g2QuickListSettingsRead(7, &rd) && rd.sw[1].known && rd.sw[1].on);
      assert(reads == 1 && sends == 2 && rd.diag.attempts == 2 && rd.diag.answeredAttempt == 1);
      assert(rd.diag.replyMs >= 1800 && rd.diag.totalMs >= 2500);
      // The same inside the resend window: the reply that came during the
      // failed send answers the read before a third attempt is built.
      sends = 0;
      reset(); replies = {Reply{}, noReply(), noReply()}; replies[0].delayMs = 1050; replies[0].silent = 1;
      onReadSend = [] {
        if (++sends != 2) return true;
        nowMs += 100; onDelay();
        return false;
      };
      assert(g2QuickListSettingsRead(7, &rd) && rd.sw[1].on && reads == 1 && sends == 2);
      assert(rd.diag.attempts == 2 && rd.diag.answeredAttempt == 1 && rd.diag.totalMs < 1200);
    }
    // A late reply to another attempt of an answered read (its state from
    // before the answer) never refreshes the cache: neither after the read nor
    // during the next one, where it counts as another reply.
    {
      reset(); replies = {Reply{}, Reply{}, noReply(), noReply(), noReply()};
      replies[0].delayMs = 1500; replies[0].silent = 1;   // attempt 1: silent on, late
      assert(g2QuickListSettingsRead(7, &rd) && rd.diag.answeredAttempt == 2 && rd.sw[1].known && !rd.sw[1].on);
      G2SettingsEcho cached{}; uint32_t cachedAt = 0;
      assert(g2SettingsEchoSnapshot(&cached, &cachedAt, 7) && !G2_ECHO_HAS(cached, 14));
      const uint32_t answeredAt = cachedAt;
      const unsigned echoes = controlEchoes;
      assert(!g2QuickListSettingsRead(7, &rd) && reads == 5 && rd.diag.otherReplies == 1);
      assert(scheduled.empty() && controlEchoes == echoes);
      assert(g2SettingsEchoSnapshot(&cached, &cachedAt, 7) && !G2_ECHO_HAS(cached, 14) && cachedAt == answeredAt);
      // Same after the read ended (current read's magics).
      reset(); replies = {Reply{}, Reply{}}; replies[0].delayMs = 1500; replies[0].silent = 1;
      assert(g2QuickListSettingsRead(7, &rd) && rd.diag.answeredAttempt == 2);
      vTaskDelay(600);
      assert(scheduled.empty());
      assert(g2SettingsEchoSnapshot(&cached, &cachedAt, 7) && !G2_ECHO_HAS(cached, 14));
      // An unanswered read's late reply is still news (published, as before).
      reset(); replies = {Reply{}, noReply(), noReply()}; replies[0].delayMs = 3500; replies[0].silent = 1;
      assert(!g2QuickListSettingsRead(7, &rd));
      vTaskDelay(600);
      assert(g2SettingsEchoSnapshot(&cached, &cachedAt, 7) && G2_ECHO_HAS(cached, 14) && cached.silentMode == 1);
    }
    // Auto brightness (sw[2]) from the same read: f18 = 1 on; a full reply
    // without f18 off (to confirm on the device); a partial one unknown.
    reset(); replies[0].autoBrightness = 1;
    assert(g2QuickListSettingsRead(7, &rd) && rd.fullProfile && rd.sw[2].known && rd.sw[2].on);
    assert(rd.sw[0].known && !rd.sw[0].on && rd.sw[1].known && !rd.sw[1].on && reads == 1 && writes == 0);
    reset();
    assert(g2QuickListSettingsRead(7, &rd) && rd.fullProfile && rd.sw[2].known && !rd.sw[2].on && reads == 1);
    reset(); replies[0].fullVersions = false;
    assert(g2QuickListSettingsRead(7, &rd) && !rd.fullProfile && !rd.sw[2].known);
    reset(); replies[0].fullVersions = false; replies[0].autoBrightness = 1;
    assert(g2QuickListSettingsRead(7, &rd) && rd.sw[2].known && rd.sw[2].on);
    reset(); assert(!g2QuickListSettingsRead(8, &rd) && reads == 0);   // another connection

    // The typed silent write: pre-read, write, ACK, read-back of f14.
    ql::WearResult w{};
    reset(); replies[1].silent = 1;
    assert(g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done);
    assert(w.acked && w.readOk && w.known && w.on && w.fullProfile && writes == 1 && reads == 2);
    // The first read-back matches: read once, kReadbackDelayMs after the ACK
    // wait (ACK seen at its first 10 ms slice), and no recheck.
    assert(readAtMs.size() == 2 && readAtMs[1] - writeAtMs >= 10 + ql::kReadbackDelayMs &&
           readAtMs[1] - writeAtMs < 10 + ql::kReadbackDelayMs + 50);
    assert(!w.rechecked && !w.recheckSkipped && !ql::readMade(w.recheck) && ql::recheckOf(w) == ql::Recheck::None);
    // Device 2026-11-21: the first read-back still says off (silent ON ACKed):
    // one recheck kReadbackRecheckMs later reads on. Two read-backs, no more.
    reset(); replies = {Reply{}, Reply{}, Reply{}}; replies[2].silent = 1;
    assert(g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done);
    assert(w.acked && w.readOk && w.known && w.on && writes == 1 && reads == 3);
    assert(w.rechecked && !w.recheckSkipped && ql::readAnswered(w.readBack) && ql::readAnswered(w.recheck));
    assert(ql::recheckOf(w) == ql::Recheck::Answered && !strcmp(ql::recheckSuffix(ql::recheckOf(w)), " after recheck"));
    assert(readAtMs[1] - writeAtMs >= 10 + ql::kReadbackDelayMs && readAtMs[1] - writeAtMs < 10 + ql::kReadbackDelayMs + 50);
    assert(readAtMs[2] - readAtMs[1] >= ql::kReadbackRecheckMs && readAtMs[2] - readAtMs[1] < ql::kReadbackRecheckMs + 50);
    // Both read-backs disagree: the row goes by the read value (off), Done.
    reset(); replies = {Reply{}, Reply{}, Reply{}};
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.acked);
    assert(w.readOk && w.known && !w.on && w.rechecked && reads == 3 && writes == 1);
    { ql::Policy pol{}; assert(ql::afterApply(pol, ql::Control::Silent, w) == ql::After::Push);
      assert(pol.row[ql::rowSlot(ql::Control::Silent)].next == ql::Shown::Off); }
    // The first read-back unanswered (three attempts): the recheck is still
    // made, 1.5 s after it, and its answer counts.
    reset(); replies = {Reply{}, noReply(), noReply(), noReply(), Reply{}}; replies[4].silent = 1;
    assert(g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.known && w.on);
    assert(reads == 5 && w.readBack.attempts == 3 && !ql::readAnswered(w.readBack));
    assert(w.recheck.attempts == 1 && ql::readAnswered(w.recheck) && ql::recheckOf(w) == ql::Recheck::Answered);
    assert(readAtMs[4] - readAtMs[3] >= 1000 + ql::kReadbackRecheckMs);   // after the last attempt's 1 s wait
    // The recheck unanswered: the first read-back's answer stands.
    reset(); replies = {Reply{}, Reply{}, noReply(), noReply(), noReply()};
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done);
    assert(w.readOk && w.known && !w.on && reads == 5 && w.recheck.attempts == 3);
    assert(ql::recheckOf(w) == ql::Recheck::Unanswered &&
           !strcmp(ql::recheckSuffix(ql::recheckOf(w)), " (recheck unanswered)"));
    // The right temple reconnects during the wait before the first read-back:
    // lost after send, nothing read back.
    reset(); rolloverAfterWriteBy(200);
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::LostAfterSend);
    assert(writes == 1 && reads == 1 && !ql::readMade(w.readBack) && !w.rechecked);
    assert(nowMs - writeAtMs < 200 + ql::kReadbackSliceMs + 10);   // given up within one slice
    // ... during the recheck wait: lost during the read-back, no second read.
    reset(); replies = {Reply{}, Reply{}, Reply{}}; replies[2].silent = 1;
    rolloverAfterWriteBy(10 + ql::kReadbackDelayMs + 300);
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::LostDuringReadback && !w.known);
    assert(writes == 1 && reads == 2 && !w.rechecked && !ql::readMade(w.recheck));
    { ql::Policy pol{}; assert(ql::afterApply(pol, ql::Control::Silent, w) == ql::After::Drop); }
    // A write that already ran past kReadbackRecheckBudgetMs (each read send
    // held 4.6 s by the TX gate) does not recheck: the job stays inside the backstop.
    {
      reset(); replies = {Reply{}, Reply{}, Reply{}};
      onReadSend = [] { nowMs += 4600; return true; };
      const uint32_t t0 = nowMs;
      assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.known && !w.on);
      assert(reads == 2 && w.recheckSkipped && !w.rechecked && ql::recheckOf(w) == ql::Recheck::Skipped);
      assert(nowMs - t0 < ql::kJobBackstopMs - ql::kJobStartDeadlineMs);
    }
    reset(); replies[0].silent = 1; emptyWearAck = true;   // captured OFF ACK: 1a 02 32 00
    assert(g2QuickListSilentWrite(7, false, &w) && w.stage == ql::WearStage::Done && w.acked && w.known && !w.on);
    reset(); replies[0].silent = 1;
    assert(g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::AlreadySet && writes == 0);
    reset(); replies[0].silent = -1;
    assert(g2QuickListSilentWrite(7, false, &w) && w.stage == ql::WearStage::AlreadySet && !w.on && writes == 0);
    reset(); replies[0].fullVersions = false;
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::PreReadFailed && writes == 0);
    reset(); replies = {noReply(), noReply(), noReply(), Reply{}};   // pre-read unanswered three times: no write
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::PreReadFailed && writes == 0);
    assert(reads == 3 && w.preRead.attempts == 3 && !ql::readAnswered(w.preRead) && !ql::readMade(w.readBack));
    // One slow pre-read reply no longer ends the write: the resend is answered.
    reset(); replies = {noReply(), Reply{}, Reply{}}; replies[2].silent = 1;
    assert(g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.known && w.on);
    assert(writes == 1 && reads == 3 && w.preRead.answeredAttempt == 2 && w.readBack.answeredAttempt == 1);
    // A read-back reply 1.2 s late (during the resend's wait) still confirms it.
    reset(); replies = {Reply{}, Reply{}, noReply()}; replies[1].delayMs = 1200; replies[1].silent = 1;
    assert(g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.on && writes == 1);
    assert(w.readBack.attempts == 2 && w.readBack.answeredAttempt == 1 && w.readBack.replyMs >= 1200);
    // A pre-read reply (pre-write state) that arrives during the read-back
    // never confirms the write: it carries the pre-read's magic.
    reset(); replies = {Reply{}, noReply(), Reply{}, noReply(), Reply{}};
    replies[0].delayMs = 3000;   // pre-read attempt 1, silent off, 3 s late
    replies[4].silent = 1;       // read-back attempt 2
    assert(g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.on && writes == 1);
    assert(w.preRead.answeredAttempt == 3 && w.readBack.answeredAttempt == 2 && w.readBack.otherReplies == 1);
    // A read-back unanswered three times: Done without a state (the owner then reads again).
    // The recheck goes unanswered too (both read-backs: three attempts each).
    reset(); replies = {Reply{}, noReply(), noReply(), noReply()};
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done && !w.readOk && !w.known);
    assert(writes == 1 && w.readBack.attempts == 3 && !ql::readAnswered(w.readBack));
    assert(reads == 7 && w.rechecked && w.recheck.attempts == 3 && ql::recheckOf(w) == ql::Recheck::Unanswered);
    reset(); replies = {Reply{}, Reply{}, Reply{}}; sendAck = false;   // not taken: read back off twice
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.known && !w.on);
    assert(!w.acked && w.rechecked && reads == 3);
    reset(); rolloverAfterWrite = true;
    assert(!g2QuickListSilentWrite(7, true, &w) && w.stage == ql::WearStage::LostAfterSend);
    // The typed auto-brightness write: pre-read, write f1{f1}, ACK with the
    // explicit zero, read-back of f18.
    reset(); replies[1].autoBrightness = 1;
    assert(g2QuickListAutoBrightWrite(7, true, &w) && w.stage == ql::WearStage::Done);
    assert(w.acked && w.readOk && w.known && w.on && w.fullProfile && writes == 1 && reads == 2 && !w.rechecked);
    // Auto brightness has no change push: a stale first read-back is fixed by the recheck.
    reset(); replies = {Reply{}, Reply{}, Reply{}}; replies[2].autoBrightness = 1;
    assert(g2QuickListAutoBrightWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.on && reads == 3);
    assert(ql::recheckOf(w) == ql::Recheck::Answered && readAtMs[2] - readAtMs[1] >= ql::kReadbackRecheckMs);
    reset(); replies[0].autoBrightness = 1;   // captured OFF ACK: 1a 04 0a 02 08 00
    assert(g2QuickListAutoBrightWrite(7, false, &w) && w.stage == ql::WearStage::Done && w.acked && w.known && !w.on);
    reset(); replies[0].autoBrightness = 1; emptyWearAck = true;   // not the captured ACK: no match
    assert(g2QuickListAutoBrightWrite(7, false, &w) && w.stage == ql::WearStage::Done && !w.acked && w.known && !w.on);
    reset(); replies[0].autoBrightness = 1;
    assert(g2QuickListAutoBrightWrite(7, true, &w) && w.stage == ql::WearStage::AlreadySet && w.on && writes == 0);
    reset();
    assert(g2QuickListAutoBrightWrite(7, false, &w) && w.stage == ql::WearStage::AlreadySet && !w.on && writes == 0);
    reset(); replies[0].fullVersions = false;
    assert(!g2QuickListAutoBrightWrite(7, true, &w) && w.stage == ql::WearStage::PreReadFailed && writes == 0);
    reset(); replies = {Reply{}, Reply{}, Reply{}}; sendAck = false;   // not taken: read back off twice
    assert(!g2QuickListAutoBrightWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.known && !w.on);
    assert(w.rechecked && reads == 3);
    reset(); rolloverAfterWrite = true;
    assert(!g2QuickListAutoBrightWrite(7, true, &w) && w.stage == ql::WearStage::LostAfterSend);
    // The wear write is unchanged.
    reset(); replies[0].wear = 1; replies[1].wear = -1; emptyWearAck = true;
    assert(g2QuickListWearWrite(7, false, &w) && w.stage == ql::WearStage::Done && w.acked && !w.on);
    assert(reads == 2 && !w.rechecked);
    // Wear has no change push either: a stale first read-back is rechecked.
    reset(); replies = {Reply{}, Reply{}, Reply{}}; replies[0].wear = replies[1].wear = 1; replies[2].wear = -1;
    emptyWearAck = true;
    assert(g2QuickListWearWrite(7, false, &w) && w.stage == ql::WearStage::Done && !w.on && reads == 3 && w.rechecked);

    // ---- head-up and brightness rows (2.3.2.14 brightness capture, 2026-11-21) ----
    // The settings read: head-up (sw[3], f7; left out of a full reply = off,
    // to confirm on the device; unknown in a partial one) and the Brightness
    // row (sw[4]): "Auto" with f18 = 1, else the level f2, else "?".
    reset(); replies[0].headUp = 1; replies[0].autoBrightness = 1; replies[0].brightness = 64;
    assert(g2QuickListSettingsRead(7, &rd) && rd.fullProfile && rd.sw[3].known && rd.sw[3].on);
    assert(rd.sw[4].known && rd.sw[4].on);
    reset(); replies[0].brightness = 60;
    assert(g2QuickListSettingsRead(7, &rd) && rd.sw[3].known && !rd.sw[3].on);
    assert(rd.sw[4].known && !rd.sw[4].on && rd.sw[4].level == 60);
    reset();   // auto brightness off and no level in the reply: "?"
    assert(g2QuickListSettingsRead(7, &rd) && rd.sw[2].known && !rd.sw[2].on && !rd.sw[4].known);
    reset(); replies[0].fullVersions = false; replies[0].brightness = 60;   // partial: f7 and f18 unknown
    assert(g2QuickListSettingsRead(7, &rd) && !rd.sw[3].known && !rd.sw[2].known && !rd.sw[4].known);
    reset(); replies[0].fullVersions = false; replies[0].headUp = 1; replies[0].autoBrightness = 1;
    assert(g2QuickListSettingsRead(7, &rd) && rd.sw[3].known && rd.sw[3].on && rd.sw[4].known && rd.sw[4].on);

    // The head-up write, off: the captured bytes (1a 04 22 02 08 00), the ACK
    // keeps the explicit zero and matches; the read-back leaves f7 out (off).
    reset(); replies[0].headUp = 1;
    assert(g2QuickListHeadUpWrite(7, false, &w) && w.stage == ql::WearStage::Done);
    assert(w.acked && w.readOk && w.known && !w.on && writes == 1 && reads == 2 && !w.rechecked);
    assert(writeLog.size() == 1 && writeLog[0].inner == 4 && writeLog[0].leaf == 1 && writeLog[0].value == 0);
    assert(hexOf(writeLog[0].payload) == capturedWrite(sentMagic[1], "22020800"));
    // ... and on.
    reset(); replies[1].headUp = 1;
    assert(g2QuickListHeadUpWrite(7, true, &w) && w.stage == ql::WearStage::Done && w.acked && w.known && w.on);
    assert(hexOf(writeLog[0].payload) == capturedWrite(sentMagic[1], "22020801"));
    // An empty f4 body is not the captured ACK (it could be the switch or the
    // angle) and never matches; the read-back alone confirms the write.
    reset(); replies[0].headUp = 1; emptyWearAck = true;
    assert(g2QuickListHeadUpWrite(7, false, &w) && w.stage == ql::WearStage::Done && !w.acked && w.known && !w.on);
    // A stale first read-back is fixed by the recheck.
    reset(); replies = {Reply{}, Reply{}, Reply{}}; replies[0].headUp = replies[1].headUp = 1;
    assert(g2QuickListHeadUpWrite(7, false, &w) && w.stage == ql::WearStage::Done && !w.on && reads == 3);
    assert(ql::recheckOf(w) == ql::Recheck::Answered);
    reset(); replies[0].headUp = 1;
    assert(g2QuickListHeadUpWrite(7, true, &w) && w.stage == ql::WearStage::AlreadySet && w.on && writes == 0);
    reset(); replies[0].fullVersions = false;
    assert(!g2QuickListHeadUpWrite(7, true, &w) && w.stage == ql::WearStage::PreReadFailed && writes == 0);
    // An auto-brightness write's result carries the level, so the Brightness
    // row follows the tick (turning auto off keeps the level it chose: 15).
    reset(); replies[0].autoBrightness = 1; replies[0].brightness = 64; replies[1].brightness = 15;
    assert(g2QuickListAutoBrightWrite(7, false, &w) && w.known && !w.on && w.levelKnown && w.level == 15);
    reset(); replies[0].autoBrightness = 1; replies[0].brightness = 64;   // already on: from the pre-read
    assert(g2QuickListAutoBrightWrite(7, true, &w) && w.stage == ql::WearStage::AlreadySet && w.levelKnown &&
           w.level == 64);

    // The Brightness tick with auto brightness on: auto OFF (captured
    // 0a 02 08 00), its ACK, then level 25 (0a 02 10 19), its ACK; one read-back
    // 500 ms after the second ACK wait confirms f2 = 25 and f18 left out (off).
    reset(); replies[0].autoBrightness = 1; replies[0].brightness = 64; replies[1].brightness = 25;
    assert(g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::Done);
    assert(w.autoWasOn && w.autoWritten && w.autoAcked && w.levelWritten && w.acked && w.target == 25);
    assert(w.readOk && w.known && !w.on && w.levelKnown && w.level == 25 && w.autoKnown && !w.autoOn);
    assert(writes == 2 && reads == 2 && !w.rechecked);
    assert(writeLog[0].inner == 1 && writeLog[0].leaf == 1 && writeLog[0].value == 0);
    assert(writeLog[1].inner == 1 && writeLog[1].leaf == 2 && writeLog[1].value == 25);
    assert(hexOf(writeLog[0].payload) == capturedWrite(sentMagic[1], "0a020800"));
    assert(hexOf(writeLog[1].payload) == capturedWrite(sentMagic[2], "0a021019"));
    assert(readAtMs[1] - writeAtMs >= 10 + ql::kReadbackDelayMs && readAtMs[1] - writeAtMs < 10 + ql::kReadbackDelayMs + 50);
    { ql::Policy pol{};   // both rows go out with the read values
      ql::rowOf(pol, ql::Control::AutoBright).shown = ql::Shown::On;
      ql::rowOf(pol, ql::Control::Brightness).shown = ql::Shown::On;
      assert(ql::afterApply(pol, ql::Control::Brightness, w) == ql::After::Push);
      assert(ql::rowOf(pol, ql::Control::AutoBright).pending && ql::rowOf(pol, ql::Control::AutoBright).next == ql::Shown::Off);
      assert(ql::rowOf(pol, ql::Control::Brightness).pending && ql::rowOf(pol, ql::Control::Brightness).nextLevel == 25);
      assert(!strcmp(ql::rowTitle(ql::Control::Brightness, ql::Shown::Off, 25).text, "Brightness: 25%")); }
    // Auto off at 60: one write, 75 (0a 02 10 4b).
    reset(); replies[0].brightness = 60; replies[1].brightness = 75;
    assert(g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::Done && w.target == 75);
    assert(!w.autoWasOn && !w.autoWritten && w.levelWritten && w.acked && writes == 1 && reads == 2);
    assert(hexOf(writeLog[0].payload) == capturedWrite(sentMagic[1], "0a02104b"));
    // 100 wraps to 25; the captured level-100 write is the same shape (0a 02 10 64).
    reset(); replies[0].brightness = 100; replies[1].brightness = 25;
    assert(g2QuickListBrightnessWrite(7, &w) && w.target == 25 && writes == 1 && writeLog[0].value == 25);
    // The glasses keep only even levels (device test 2026-11-21): a tick to 25
    // reads back 24, and that is confirmed on the first read-back (no recheck).
    reset(); replies[0].brightness = 100; replies[1].brightness = 24;
    assert(g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::Done && w.target == 25);
    assert(w.level == 24 && reads == 2 && !w.rechecked && ql::recheckOf(w) == ql::Recheck::None);
    { ql::Policy pol{};   // the row shows the step: "Brightness: 25%"
      assert(ql::afterApply(pol, ql::Control::Brightness, w) == ql::After::Push);
      const ql::Row& row = ql::rowOf(pol, ql::Control::Brightness);
      assert(!strcmp(ql::rowTitle(ql::Control::Brightness, row.next, row.nextLevel).text, "Brightness: 25%")); }
    // The next tick from 24 writes 50, and from 74 writes 100 (75 read back 74).
    reset(); replies[0].brightness = 24; replies[1].brightness = 50;
    assert(g2QuickListBrightnessWrite(7, &w) && w.target == 50 && writes == 1 && writeLog[0].value == 50 && reads == 2);
    reset(); replies[0].brightness = 50; replies[1].brightness = 74;
    assert(g2QuickListBrightnessWrite(7, &w) && w.target == 75 && reads == 2 && !w.rechecked);
    reset(); replies[0].brightness = 74; replies[1].brightness = 100;
    assert(g2QuickListBrightnessWrite(7, &w) && w.target == 100 && writeLog[0].value == 100);
    // 25 -> 50, 50 -> 75, 75 -> 100.
    for (int from : {25, 50, 75}) {
      reset(); replies[0].brightness = from; replies[1].brightness = from + 25;
      assert(g2QuickListBrightnessWrite(7, &w) && w.target == from + 25 && writeLog[0].value == uint32_t(from + 25));
    }
    reset(); replies[0].brightness = 75; replies[1].brightness = 100;
    assert(g2QuickListBrightnessWrite(7, &w) && hexOf(writeLog[0].payload) == capturedWrite(sentMagic[1], "0a021064"));
    // Auto off and no level in the pre-read: the first step, 25.
    reset(); replies[1].brightness = 25;
    assert(g2QuickListBrightnessWrite(7, &w) && w.target == 25 && writes == 1);
    // The auto-off write is not ACKed: no level write; the read-back still says
    // auto on, so it is rechecked once, and still on: the honest result,
    // AutoOffUnacked, false.
    reset(); replies = {Reply{}, Reply{}, Reply{}};
    replies[0].autoBrightness = replies[1].autoBrightness = replies[2].autoBrightness = 1;
    replies[0].brightness = 64; replies[1].brightness = replies[2].brightness = 15;
    ackIf = [](uint8_t inner, uint8_t leaf, uint32_t) { return !(inner == 1 && leaf == 1); };
    assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::AutoOffUnacked);
    assert(w.autoWritten && !w.autoAcked && !w.levelWritten && !w.acked && writes == 1 && reads == 3);
    assert(w.readOk && w.known && w.on && w.autoKnown && w.autoOn && w.rechecked);
    assert(ql::recheckOf(w) == ql::Recheck::Answered);
    { ql::Policy pol{};
      ql::rowOf(pol, ql::Control::AutoBright).shown = ql::Shown::On;
      ql::rowOf(pol, ql::Control::AutoBright).acked = true;
      assert(ql::afterApply(pol, ql::Control::Brightness, w) == ql::After::Push);
      assert(ql::rowOf(pol, ql::Control::Brightness).next == ql::Shown::On);
      assert(!ql::rowOf(pol, ql::Control::AutoBright).pending); }
    // The auto-off ACK comes too late (or the glasses apply it slowly): no
    // level write, the first read-back still says on, the recheck 1.5 s later
    // shows auto off at the level it chose (15). Both rows show that.
    reset(); replies = {Reply{}, Reply{}, Reply{}};
    replies[0].autoBrightness = replies[1].autoBrightness = 1;
    replies[0].brightness = 64; replies[1].brightness = replies[2].brightness = 15;
    ackIf = [](uint8_t inner, uint8_t leaf, uint32_t) { return !(inner == 1 && leaf == 1); };
    assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::AutoOffUnacked);
    assert(!w.levelWritten && writes == 1 && reads == 3 && w.rechecked);
    assert(readAtMs[2] - readAtMs[1] >= ql::kReadbackRecheckMs && readAtMs[2] - readAtMs[1] < ql::kReadbackRecheckMs + 50);
    assert(w.readOk && w.known && !w.on && w.autoKnown && !w.autoOn && w.levelKnown && w.level == 15);
    { ql::Policy pol{};
      ql::rowOf(pol, ql::Control::AutoBright).shown = ql::Shown::On;
      ql::rowOf(pol, ql::Control::AutoBright).acked = true;
      ql::rowOf(pol, ql::Control::Brightness).shown = ql::Shown::On;
      assert(ql::afterApply(pol, ql::Control::Brightness, w) == ql::After::Push);
      assert(ql::rowOf(pol, ql::Control::Brightness).pending && ql::rowOf(pol, ql::Control::Brightness).next == ql::Shown::Off);
      assert(ql::rowOf(pol, ql::Control::Brightness).nextLevel == 15);
      assert(ql::rowOf(pol, ql::Control::AutoBright).pending && ql::rowOf(pol, ql::Control::AutoBright).next == ql::Shown::Off); }
    // Not ACKed and the read-back already shows auto off: no recheck.
    reset(); replies[0].autoBrightness = 1; replies[0].brightness = 64; replies[1].brightness = 15;
    ackIf = [](uint8_t inner, uint8_t leaf, uint32_t) { return !(inner == 1 && leaf == 1); };
    assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::AutoOffUnacked);
    assert(reads == 2 && !w.rechecked && w.known && !w.on && w.level == 15);
    // The level write's ACK missing: the read-back still confirms it.
    reset(); replies[0].brightness = 50; replies[1].brightness = 75;
    ackIf = [](uint8_t, uint8_t leaf, uint32_t) { return leaf != 2; };
    assert(g2QuickListBrightnessWrite(7, &w) && w.levelWritten && !w.acked && w.level == 75);
    // Read-back mismatch, then the recheck 1.5 s later shows the level.
    reset(); replies = {Reply{}, Reply{}, Reply{}};
    replies[0].brightness = replies[1].brightness = 50; replies[2].brightness = 75;
    assert(g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::Done && w.level == 75 && reads == 3);
    assert(w.rechecked && ql::recheckOf(w) == ql::Recheck::Answered);
    assert(readAtMs[2] - readAtMs[1] >= ql::kReadbackRecheckMs && readAtMs[2] - readAtMs[1] < ql::kReadbackRecheckMs + 50);
    // ... and when the recheck still disagrees: Done, false, the read value.
    reset(); replies = {Reply{}, Reply{}, Reply{}};
    replies[0].brightness = replies[1].brightness = replies[2].brightness = 50;
    assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::Done && w.known && w.level == 50);
    // Auto turned off and the level written, but the read-back still says auto
    // on: not confirmed, rechecked; the recheck shows it off.
    reset(); replies = {Reply{}, Reply{}, Reply{}};
    replies[0].autoBrightness = replies[1].autoBrightness = 1; replies[0].brightness = 64;
    replies[1].brightness = 25; replies[2].brightness = 25;
    assert(g2QuickListBrightnessWrite(7, &w) && w.rechecked && w.autoKnown && !w.autoOn && reads == 3 && writes == 2);
    // The link goes after the auto-off write: no level write, lost after send.
    reset(); replies[0].autoBrightness = 1; replies[0].brightness = 64; rolloverAfterWrite = true;
    assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::LostAfterSend && writes == 1);
    // No full pre-read (or none at all): nothing written.
    reset(); replies[0].fullVersions = false; replies[0].brightness = 60;
    assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::PreReadFailed && writes == 0);
    reset(); replies = {noReply(), noReply(), noReply()};
    assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::PreReadFailed && writes == 0 && reads == 3);
    // The read-back unanswered (both read-backs, three attempts each): Done
    // without a state, so the owner reads every row again.
    reset(); replies = {Reply{}, noReply(), noReply(), noReply(), noReply(), noReply(), noReply()};
    replies[0].brightness = 50;
    assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::Done && !w.readOk && !w.known);
    { ql::Policy pol{}; assert(ql::afterApply(pol, ql::Control::Brightness, w) == ql::After::ReadThenPush); }
    // Every TX send held 4.6 s: the two-write job skips its recheck and ends
    // well inside the (30 s) backstop.
    {
      reset(); replies = {Reply{}, Reply{}, Reply{}};
      replies[0].autoBrightness = replies[1].autoBrightness = 1; replies[0].brightness = 64;
      onReadSend = [] { nowMs += 4600; return true; };
      const uint32_t t0 = nowMs;
      assert(!g2QuickListBrightnessWrite(7, &w) && w.stage == ql::WearStage::Done && w.recheckSkipped);
      assert(writes == 2 && reads == 2 && nowMs - t0 < ql::kJobBackstopMs - ql::kJobStartDeadlineMs);
    }

    // The wire gate: only the five rows' exact FULL_UPDATE, exact pair, right temple.
    uint8_t pb[ql::kPbMax];
    const char* titles[ql::kRowCount] = {"Wear Detection: On", "Silent Mode: Off", "Auto Brightness: ?",
                                         "Head-Up Display: Off", "Brightness: 25%"};
    reset(); quickListSends = 0;
    size_t n = ql::encodeRows(1, titles, pb, sizeof pb);
    assert(n > 128 && g2QuickListWireSend(7, pb, n) && quickListSends == 1);
    n = ql::encodeRows(200, titles, pb, sizeof pb);   // a two-byte magic
    assert(n && g2QuickListWireSend(7, pb, n) && quickListSends == 2);
    {   // rows out of order (Silent at index 0): refused
      ql::ListItem items[ql::kRowCount];
      for (uint8_t i = 0; i < ql::kRowCount; ++i) items[i] = ql::ListItem{ql::rowUid((ql::Control)i), i, titles[i]};
      items[0].uid = ql::kSilentUid; items[1].uid = ql::kWearUid;
      n = ql::encodeFullUpdate(3, items, ql::kRowCount, pb, sizeof pb);
      assert(n && !g2QuickListWireSend(7, pb, n) && quickListSends == 2);
      // four rows only: refused
      for (uint8_t i = 0; i < ql::kRowCount; ++i) items[i] = ql::ListItem{ql::rowUid((ql::Control)i), i, titles[i]};
      n = ql::encodeFullUpdate(4, items, ql::kRowCount - 1, pb, sizeof pb);
      assert(n && !g2QuickListWireSend(7, pb, n) && quickListSends == 2);
      // a foreign uid in place of a row: refused
      items[4].uid = ql::kUidBase + 6;
      n = ql::encodeFullUpdate(5, items, ql::kRowCount, pb, sizeof pb);
      assert(n && !g2QuickListWireSend(7, pb, n) && quickListSends == 2);
    }
    n = ql::encodeRows(6, titles, pb, sizeof pb);
    snprintf(gL.firmwareVersion, sizeof(gL.firmwareVersion), "2.3.2.13");
    assert(!g2QuickListWireSend(7, pb, n) && quickListSends == 2);
    G2QuickListLinkState link{};
    reset(); assert(g2QuickListLink(&link) && link.firmwareExact && link.rightGeneration == 7 && !link.busy);
  }

  // A late unsolicited snapshot must not erase the requested refresh result.
  reset(); replies[0].wear = -1; replies[0].asyncAfter = true;
  run("refresh"); assert(has(broadcast, "weardetect  off"));
  assert(g2SettingsEchoSnapshot(&cached, nullptr, 7, &full) && !full);
  // A single-knob read of that partial snapshot reads again, like the bare show;
  // the next correlated reply answers it.
  { const unsigned before = reads;
    assert(has(run("weardetect"), "= off") && reads == before + 1); }
  // With no answer to that read it stays unreported.
  replies.resize(reads);
  { Reply partial; partial.wear = -1; partial.flag = G2_FLAG_NOTIFY; replyRead(250, partial); }
  assert(has(run("weardetect"), "not reported"));

  // Scalar absence is unknown in partial replies, not silently false.
  // Writing off again: the glasses send no ACK for a value they already hold
  // and leave an off scalar out of the answered read. That is a plain report.
  const char* const kNoOpReport =
      "G2 glasses: weardetect = 0 sent, unconfirmed. No matching ACK came and the fresh readback "
      "does not report this setting. The glasses do not acknowledge a value they already hold.";
  reset(); replies[1].wear = -1; replies[1].fullVersions = false; sendAck = false;
  assert(run() == kNoOpReport);
  assert(has(run("weardetect"), "not reported"));
  reset(); replies[1].wear = -1; replies[1].version = "2.2.6.10"; sendAck = false;
  assert(run() == kNoOpReport);
  reset("2.2.6.10"); replies[0].version = replies[1].version = "2.2.6.10";
  replies[1].wear = -1; sendAck = false;
  assert(run() == kNoOpReport && writes == 1);
  // The same answered read is a failure for a value that is not 0, and so is a
  // read the glasses did not answer.
  reset(); replies[1].wear = -1; replies[1].fullVersions = false; sendAck = false;
  { const auto result = run("weardetect on"); assert(failed(result) && has(result, "unconfirmed") && has(result, "does not report this setting") && writes == 1); }
  reset(); replies[1].send = false; sendAck = false;
  { const auto result = run(); assert(failed(result) && has(result, "unconfirmed") && writes == 1); }
  reset(); replies[1].send = false;
  assert(has(run(), "acked by RIGHT glasses; fresh readback unavailable"));
  reset(); replies[1].wear = 1;
  { const auto result = run(); assert(failed(result) && has(result, "fresh readback is 1 despite matching ACK")); }

  // The glasses move the brightness level themselves while auto brightness is
  // on. An acknowledged level that reads back as another one is then a plain
  // report. Without the ACK, or with auto brightness off, it is a failure.
  auto brightnessCase = [](int autoBrightness, bool ack) {
    reset("2.2.4.34"); replies[0].version = replies[1].version = "2.2.4.34";
    replies[1].brightness = 78; replies[1].autoBrightness = autoBrightness; sendAck = ack;
    const std::string result = run("brightness 80"); assert(writes == 1);
    return result;
  };
  assert(brightnessCase(1, true) ==
         "G2 glasses: brightness = 80 acked by RIGHT glasses. Fresh readback is 78: "
         "auto brightness is on and moves the level by itself.");
  assert(failed(brightnessCase(0, true)) && failed(brightnessCase(-1, true)));
  assert(has(brightnessCase(0, true), "requested 80, fresh readback is 78 despite matching ACK"));
  assert(failed(brightnessCase(1, false)));

  // Bad first responses must never admit a candidate write.
  for (unsigned fault = 0; fault < 7; ++fault) {
    reset();
    if (fault == 0) replies[0].send = false;
    if (fault == 1) replies[0].magicDelta = 1;
    if (fault == 2) replies[0].flag = G2_FLAG_NOTIFY;
    if (fault == 3) replies[0].side = 'L';
    if (fault == 4) replies[0].generationDelta = -1;
    if (fault == 5) replies[0].fullVersions = false;
    if (fault == 6) replies[0].version = "2.3.2.13";
    assert(has(run(), "wear write not sent")); assert(writes == 0);
  }
  // A prior cached full reply cannot prime a new transaction.
  reset(); replyRead(250, Reply{}); replies[0].send = false;
  assert(has(run(), "wear write not sent") && writes == 0);

  // No wrong tuple/arm/flag/epoch can masquerade as a matching write ACK.
  for (unsigned fault = 0; fault < 8; ++fault) {
    reset(); replies[1].send = false;
    if (fault == 0) wrongAckValue = true;
    if (fault == 1) ackSide = 'L';
    if (fault == 2) ackFlag = G2_FLAG_NOTIFY;
    if (fault == 3) ackGenerationDelta = -1;
    if (fault == 4) ackMagicDelta = 1;
    if (fault == 5) sendAck = false;
    if (fault == 6) wrongAckInner = true;
    if (fault == 7) wrongAckLeaf = true;
    const auto result = run(); assert(has(result, "unconfirmed") && !has(result, "acked by RIGHT"));
  }
  reset(); rolloverAfterWrite = true;
  assert(has(run(), "connection changed after send; result unconfirmed"));
  assert(!g2SettingsEchoSnapshot(&cached, nullptr));
  reset(); rolloverAtReadback = true;
  assert(has(run(), "connection changed during readback; result unconfirmed"));

  // An on/off setting takes the shared on/off words; any other word sends nothing.
  reset();
  assert(has(run("weardetect disable"), "confirmed by fresh RIGHT readback and matching ACK") && writes == 1);
  reset(); assert(has(run("weardetect maybe"), "Error:") && writes == 0);

  reset(); gR.sessionReadyGeneration = 0;
  assert(has(run(), "right native session not ready") && writes == 0);
  reset(); gR.writeChar = nullptr;
  assert(has(run(), "right native session not ready") && writes == 0);
  reset(); gR.connected = false;
  assert(has(run(), "right native session not ready") && writes == 0);
  // 2.3.2.14 admits weardetect, silent, autobright, headup and brightness only
  // (silent and autobright left this list when their read-backs, BASIC f14 and
  // f18, and writes were captured on 2026-11-21; headup and the brightness
  // level with the brightness capture the same day). The head-up angle stays refused.
  for (const char* command : {"headangle 20", "y 5", "x 1",
                              "unitformat 1", "distunit 1", "timeformat 1", "dateformat 1", "tempunit 1"}) {
    reset(); assert(has(run(command), "not enabled") && writes == 0);
    assert(has(run(command), "weardetect, silent, autobright, headup and brightness only"));
  }
  reset(); snprintf(gL.firmwareVersion, sizeof(gL.firmwareVersion), "2.3.2.13");
  assert(has(run(), "not enabled") && writes == 0);
  reset("2.2.4.34"); replies[0].version = replies[1].version = "2.2.4.34";
  assert(has(run("brightness 10"), "acked by RIGHT") && writes == 1);
  reset(""); replies[0].version = replies[1].version = "2.2.4.34";
  assert(has(run("brightness 10"), "acked by RIGHT") && writes == 1);
  reset("2.2.4.34"); assert(has(run("headup off"), "require both temples") && writes == 0);

  reset("");
  Reply partial; partial.fullVersions = false;
  replyRead(250, partial); // Fresh display cache without learned firmware.
  replies[0].version = replies[1].version = "2.2.4.34";
  assert(has(run("brightness 10"), "acked by RIGHT") && writes == 1);

  // Byte IDs wrap without repeating adjacent calls or colliding with policy242.
  uint8_t previous = g2NextSettingsMagic();
  for (unsigned i = 0; i < 700; ++i) {
    uint8_t magic = g2NextSettingsMagic();
    assert(magic && magic != 242 && magic != previous); previous = magic;
  }

  // Actual exact-generation sender + actual per-ATT physical fence.
  uint8_t wire[64]; const size_t bytes = g2BuildSettingInfoWrite(1, 222, 5, 1, 0, wire, sizeof(wire));
  reset(); onGate = [] { ++gR.connectionGeneration; gR.sessionReadyGeneration = gR.connectionGeneration; };
  assert(!sendEnvelopeExactGeneration(gR, wire, bytes, 7, "2.3.2.14") && physicalWrites == 0);
  reset(); onGate = [] { snprintf(gL.firmwareVersion, sizeof(gL.firmwareVersion), "2.3.2.13"); };
  assert(!sendEnvelopeExactGeneration(gR, wire, bytes, 7, "2.3.2.14") && physicalWrites == 0);
  reset(); onStats = [] { ++gR.connectionGeneration; gR.sessionReadyGeneration = gR.connectionGeneration; };
  assert(!sendEnvelopeExactGeneration(gR, wire, bytes, 7, "2.3.2.14") && physicalWrites == 0);
  reset(); gR.mtu = 23;
  afterPhysical = [] { ++gR.connectionGeneration; gR.sessionReadyGeneration = gR.connectionGeneration; };
  uint8_t large[80] = {};
  assert(!sendEnvelopeNoMutex(gR, large, sizeof(large), 0, nullptr, 0, 0, 7));
  assert(physicalWrites == 1);

  // A fragmented reader REBUILD must not paint over a replacement surface,
  // including replacement between physical ATT writes of one envelope.
  reset(); gR.mtu = 23;
  afterPhysical = [] { ++presentationEpoch; };
  assert(!sendEnvelopeNoMutex(gR, large, sizeof(large), 0, nullptr, 0, 0, 7, 0, 1));
  assert(physicalWrites == 1);
  reset(); onStats = [] { ++presentationEpoch; };
  assert(!sendEnvelopeNoMutex(gR, large, sizeof(large), 0, nullptr, 0, 0, 7, 0, 1));
  assert(physicalWrites == 0);

  // Native Terminal revocation is checked by the actual sender adjacent to
  // each physical ATT write, even when one envelope needs multiple writes.
  reset(); gR.mtu = 23;
  afterPhysical = [] { nativeTerminalEpoch = 0; };
  assert(!sendEnvelopeNoMutex(gR, large, sizeof(large), 0, nullptr, 0, 0, 7, 0, 0, 73));
  assert(physicalWrites == 1);
  reset(); onStats = [] { nativeTerminalEpoch = 0; };
  assert(!sendEnvelopeNoMutex(gR, large, sizeof(large), 0, nullptr, 0, 0, 7, 0, 0, 73));
  assert(physicalWrites == 0);

  puts("G2 native settings command: correlation, default OFF, cache, generation/ATT fencing, policy isolation passed");
}
