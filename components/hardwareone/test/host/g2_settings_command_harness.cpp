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
static void vTaskDelay(unsigned n) { nowMs += n; }
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
using SemaphoreHandle_t = int*;
static constexpr int pdTRUE = 1;
static std::function<void()> onGate, onStats;
static bool bleCentralTxTake(unsigned) { if (onGate) onGate(); return true; }
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
  return gen && g2TempleReadyAtGeneration(t, gen) &&
      sendEnvelopeNoMutex(t, data, len, 0, nullptr, 0, 0, gen);
}

// Replies describe independent wire fixtures; they do not emulate admission or
// confirmation logic, which is supplied by the production owner above.
struct Reply {
  bool send = true, fullVersions = true, asyncAfter = false;
  int wear = 0; // -1 = absent nanopb scalar
  uint8_t flag = G2_FLAG_RESPONSE;
  int magicDelta = 0;
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
static std::function<void()> afterPhysical;

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
  if (reply.fullVersions) {
    assert(g2PbWriteString(body, sizeof(body), &pos, 5, reply.version));
    assert(g2PbWriteString(body, sizeof(body), &pos, 6, reply.version));
  }
  if (reply.wear >= 0) assert(g2PbWriteUint32(body, sizeof(body), &pos, 10, reply.wear));
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
    ++reads; sentMagic.push_back(uint8_t(magic));
    if (rolloverAtReadback && writes) {
      ++gR.connectionGeneration; gR.sessionReadyGeneration = gR.connectionGeneration;
      g2SettingsEchoInvalidateFreshness();
    }
    if (reads <= replies.size()) replyRead(uint8_t(magic), replies[reads - 1]);
    return true;
  }
  uint8_t inner = 0, leaf = 0; uint32_t value = 0;
  assert(g2ParseSettingWriteAck(frame.payload, frame.payloadLen, &magic, &inner, &leaf, &value));
  ++writes; sentMagic.push_back(uint8_t(magic));
  if (rolloverAfterWrite) {
    ++gR.connectionGeneration; gR.sessionReadyGeneration = gR.connectionGeneration;
    g2SettingsEchoInvalidateFreshness();
  }
  if (sendAck) {
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
  sentMagic.clear(); broadcast.clear(); onGate = onStats = afterPhysical = nullptr;
}
static bool has(const std::string& text, const char* fragment) { return text.find(fragment) != std::string::npos; }
static std::string run(const char* command = "weardetect off") { return cmd_g2glasses(String(command)); }

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

  // A late unsolicited snapshot must not erase the requested refresh result.
  reset(); replies[0].wear = -1; replies[0].asyncAfter = true;
  run("refresh"); assert(has(broadcast, "weardetect  off"));
  assert(g2SettingsEchoSnapshot(&cached, nullptr, 7, &full) && !full);
  assert(has(run("weardetect"), "not reported"));

  // Scalar absence is unknown in partial replies, not silently false.
  reset(); replies[1].wear = -1; replies[1].fullVersions = false; sendAck = false;
  assert(has(run(), "unconfirmed"));
  assert(has(run("weardetect"), "not reported"));
  reset(); replies[1].wear = -1; replies[1].version = "2.2.6.10"; sendAck = false;
  assert(has(run(), "unconfirmed"));
  reset(); replies[1].send = false;
  assert(has(run(), "acked by RIGHT glasses; fresh readback unavailable"));
  reset(); replies[1].wear = 1;
  assert(has(run(), "fresh readback is 1 despite matching ACK"));

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

  reset(); gR.sessionReadyGeneration = 0;
  assert(has(run(), "right native session not ready") && writes == 0);
  reset(); gR.writeChar = nullptr;
  assert(has(run(), "right native session not ready") && writes == 0);
  reset(); gR.connected = false;
  assert(has(run(), "right native session not ready") && writes == 0);
  for (const char* command : {"brightness 10", "headup off", "headangle 20", "silent off"}) {
    reset(); assert(has(run(command), "not enabled") && writes == 0);
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
