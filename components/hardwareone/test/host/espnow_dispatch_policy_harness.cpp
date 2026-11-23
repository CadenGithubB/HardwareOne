// Host harness for test_espnow_dispatch_policy.py. The runner inserts the
// production opcode/flag enums and header (System_ESPNow_Wire.h slices),
// V4RxCtx, LogMessageType and TextQueueEntry (System_ESPNow.h), the
// V4_OPC_FLAG_* constants and V4OpcodeEntry, espnowOriginIsKnown and the real
// v4h_text, one recording double per other handler named in kV4HandlerTable,
// the table itself, v4_dispatch_lookup, v4_dispatch_table_try and the
// espnow_task text-queue drain at the INSERT_* tags, and defines
// ENABLE_BONDED_MODE on the command line (compiled once per value). The bond
// cases also get the real isBondPeerMac (when the source has it) and the list
// of bond rows.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "System_MacText.h"  // production MAC text helpers (copied beside this unit)

#ifndef ENABLE_BONDED_MODE
#error "ENABLE_BONDED_MODE must be defined by the runner"
#endif

struct wifi_pkt_rx_ctrl_t { int8_t rssi; };
struct esp_now_recv_info { uint8_t* src_addr; uint8_t* des_addr; wifi_pkt_rx_ctrl_t* rx_ctrl; };

// INSERT_WIRE

// INSERT_RXCTX

static std::string gLastHandler;
static unsigned gHandlerCalls = 0;
static unsigned gCleanups = 0;
static unsigned gWarns = 0;
static unsigned gPrints = 0;
static uint16_t gLastPayloadLen = 0;

static void noteHandler(const char* name, const V4RxCtx& ctx) {
  gLastHandler = name;
  gLastPayloadLen = ctx.payloadLen;
  ++gHandlerCalls;
}

// ---- Platform doubles for v4h_text and the drain ----------------------------
class String {
  std::string text_;
 public:
  String() = default;
  String(const char* s) : text_(s ? s : "") {}
  size_t length() const { return text_.size(); }
  const char* c_str() const { return text_.c_str(); }
};
template <class... A> static void debugSink(A&&...) {}
#define DEBUG_ESPNOW_ROUTERF(...) debugSink(__VA_ARGS__)
#define BROADCAST_PRINTF(...) do { ++gPrints; debugSink(__VA_ARGS__); } while (0)
#define DEBUG_ESPNOW_ROUTER 1
#define ESPNOW_TEXT_MAX_LEN 1024

#define WARN_ESPNOWF(...) do { ++gWarns; debugSink(__VA_ARGS__); } while (0)
[[maybe_unused]] static struct { bool bondModeEnabled = true; String bondPeerMac; } gSettings;

// INSERT_MESSAGE_TYPES

// INSERT_TEXT_QUEUE_ENTRY

// Owner state double: only the members the extracted code touches.
struct EspNowDevice { uint8_t mac[6]; String name; };
struct EspNowState {
  static constexpr int TEXT_QUEUE_SIZE = 16;
  TextQueueEntry textQueue[TEXT_QUEUE_SIZE] = {};
  volatile int textQueueHead = 0;
  volatile int textQueueTail = 0;
  EspNowDevice devices[4] = {};
  int deviceCount = 0;
  bool initialized = true;
  bool encryptionEnabled = false;
};
static EspNowState gState;
static EspNowState* gEspNow = &gState;

// ---- Mesh, registry, history and event-bus doubles --------------------------
enum class EspNowMeshRxKind : uint8_t { RxActivity, MeshHeartbeat };
static bool meshEnabled() { return false; }
static void noteMeshPeerRxActivity(const uint8_t*, EspNowMeshRxKind, int8_t, int8_t, bool = false) {}
static bool meshRouterLookup(const uint8_t*, uint8_t*, uint8_t*) { return false; }
static String macToDisplayStr(const uint8_t* mac) {  // System_Utils.h
  char b[18];
  macToDisplay(mac, b, sizeof b);
  return String(b);
}
// Registry double: the paired name for a MAC in devices[] (the production
// lookup also consults mesh metadata; the decision under test is in v4h_text).
static String getEspNowDeviceName(const uint8_t* mac) {
  for (int i = 0; i < gEspNow->deviceCount; ++i) {
    if (std::memcmp(gEspNow->devices[i].mac, mac, 6) == 0) return gEspNow->devices[i].name;
  }
  return String();
}
// System_ESPNow.h: how a received record's sender is shown (the drain passes
// Attributed for authenticated frames, Unattributed otherwise).
enum class HistoryAttribution : uint8_t { FromName = 0, Attributed = 1, Unattributed = 2 };
struct StoredMessage {
  std::string mac, name, content; bool encrypted; int type; uint32_t reqId; uint8_t piece, of;
  HistoryAttribution attribution;
};
static std::vector<StoredMessage> gStored;
static bool gStoreRefuses = false;  // the history refused the record (full, no memory)
static bool storeMessageInPeerHistory(uint8_t* mac, const char* name, const char* message, bool encrypted,
                                      LogMessageType type, uint32_t reqId = 0, uint8_t piece = 1,
                                      uint8_t pieceTotal = 1,
                                      HistoryAttribution attribution = HistoryAttribution::FromName) {
  if (gStoreRefuses) return false;
  gStored.push_back({macToDisplayStr(mac).c_str(), name ? name : "", message ? message : "",
                     encrypted, (int)type, reqId, piece, pieceTotal, attribution});
  return true;
}
static unsigned gPushKicks = 0;
static void g2EspNowAppOnRxText(const uint8_t*) { ++gPushKicks; }
static constexpr uint8_t SYSEVT_TEXT_RX = 7;
struct PostedEvent { uint8_t kind; std::string subject, detail; };
static std::vector<PostedEvent> gEvents;
static void systemEventPost(uint8_t kind, const char* subject = nullptr, const char* detail = nullptr,
                            uint8_t = 0, const char* = nullptr) {
  gEvents.push_back({kind, subject ? subject : "", detail ? detail : ""});
}

// ---- Bond partner gate: doubles for isBondPeerMac and the dispatcher ---------
// The firmware parseMacAddress needs the Arduino String. This double accepts
// the same hand-typed separators and counts its calls, so the cases can show
// that the DISPLAY form bondconnect stores is answered without it (the firmware
// parser allocates; the gate runs for every received bond frame).
static unsigned gParseCalls = 0;
static bool parseMacAddress(const String& text, uint8_t mac[6]) {
  ++gParseCalls;
  return macParse(text.c_str(), mac);
}
static uint32_t gNowMs = 1;
static uint32_t millis() { return gNowMs; }

// Bond partner change hold and the handshake back-off (both tested by their
// own suites): no change under way, and hearing a peer has no effect here.
[[maybe_unused]] static bool bondPartnerChanging() { return false; }
[[maybe_unused]] static void espnowHandshakeHeard(const uint8_t*) {}
// The bond agreement's hook in the drop path (test_espnow_bond_agreement.py
// runs the real one): who was noted, and whether the frame came over a session.
static unsigned gIgnoredNotes = 0;
static uint8_t gIgnoredFrom[6] = {};
static bool gIgnoredOverSession = false;
[[maybe_unused]] static void bondAgreementNoteIgnoredFrame(const uint8_t* src, bool sessionEncrypted) {
  ++gIgnoredNotes;
  std::memcpy(gIgnoredFrom, src, 6);
  gIgnoredOverSession = sessionEncrypted;
}

// INSERT_BOND_PEER

// INSERT_POLICY

// INSERT_ORIGIN_KNOWN

// Calls of the BOOT notice's session rule (sessionPeerBootNotice double, made
// by the driver when the tree's BOOT row has its own handler), of the record
// of a counter seen in a sealed notice (sessionPeerBootSeen double) and of the
// SESSION_UNKNOWN rule (sessionUnknownHintReceived double, when the tree has
// that row).
[[maybe_unused]] static unsigned gBootRuleCalls = 0;
[[maybe_unused]] static unsigned gBootSeenCalls = 0;
[[maybe_unused]] static unsigned gHintRuleCalls = 0;
// The verdict the sessionPeerBootNotice double answers (PeerBootVerdict:
// 0 DropSession, 4 SameBoot, 5 OlderBoot).
[[maybe_unused]] static int gBootVerdict = 0;
// The header meshFingerprint deliver() stamps, and the fingerprint of the mesh
// gPeerA is paired in (the fingerprintForPeer double). Any other MAC reads as
// the default mesh's.
static constexpr uint16_t kPeerAMeshFp = 0x4A21;
static constexpr uint16_t kOtherMeshFp = 0x77C0;
static uint16_t gFrameFp = kPeerAMeshFp;
[[maybe_unused]] static uint16_t fingerprintForPeer(const uint8_t* mac) {
  static const uint8_t peerA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01};
  return std::memcmp(mac, peerA, 6) == 0 ? kPeerAMeshFp : (uint16_t)0x1111;
}
#define INFO_ESPNOWF(...) debugSink(__VA_ARGS__)
[[maybe_unused]] static const char* MAC_STR(const uint8_t*) { return "mac"; }
// INSERT_TEXT_HANDLER

// INSERT_HANDLER_STUBS

// INSERT_TABLE

// INSERT_LOOKUP

// The production cleanup walks the reassembly table; this double only counts.
static void v4_dispatch_post_cleanup(const esp_now_recv_info*, const EspNowV4Header*) { ++gCleanups; }

// The receive-drop counter of System_ESPNow.cpp (espnowstats rxLengthDrops).
static uint32_t gEspNowRxLenDrops = 0;

// Channel follow hook (System_ESPNow.cpp): no follower in this harness.
static void espnowChFollowOnSealedFrame(const esp_now_recv_info*, uint8_t) {}

// The reassembly step (System_ESPNow.cpp v4_reasm_step): every frame here is
// a single frame, so it is never called; test_espnow_reassembly.py runs the
// real one.
[[maybe_unused]] static bool v4_reasm_step(const esp_now_recv_info*, const EspNowV4Header*, const uint8_t*&,
                                           uint16_t&, V4Verdict*) { return true; }
[[maybe_unused]] static void v4_reasm_refused_note(const uint8_t*, uint32_t, uint8_t) {}

// INSERT_DISPATCH

// The last verdict the dispatcher returned (1.0 step 5b). A frame is
// "claimed" when its opcode has a row: everything but kUnsupported.
static V4Verdict gLastVerdict{};
static bool v4_dispatch_claims(const esp_now_recv_info* ri, const EspNowV4Header* h, const uint8_t* payload,
                               uint16_t len, bool paired, bool authed, bool enc, const char* name) {
  gLastVerdict = v4_dispatch_table_try(ri, h, payload, len, paired, authed, enc, name);
  return !(gLastVerdict.kind == V4VerdictKind::Refused && gLastVerdict.status == espnowanswer::kUnsupported);
}

// INSERT_DRAIN

static const struct { uint8_t opcode; const char* name; } kOpcodeNames[] = {
// INSERT_OPCODE_NAMES
};

static const char* opcodeName(uint8_t opcode) {
  for (const auto& entry : kOpcodeNames) {
    if (entry.opcode == opcode) return entry.name;
  }
  return "unnamed";
}

// Rows that legitimately carry no table flag: handshakes that establish trust
// themselves, plaintext-compatible beacons and the topology opcodes whose
// handlers enforce their own gates. A subset check: a row may leave this
// list, but no new zero-flag row may appear outside it.
static const uint8_t kZeroFlagAllowed[] = {
// INSERT_ZERO_FLAG_ALLOWED
};

static bool zeroFlagAllowed(uint8_t opcode) {
  for (uint8_t allowed : kZeroFlagAllowed) {
    if (allowed == opcode) return true;
  }
  return false;
}

static uint8_t gSrc[6] = {2, 4, 6, 8, 10, 12};

// Dispatches one single-fragment frame of `type` with the given trust bits and
// reports whether exactly one handler ran.
static bool run(uint8_t type, bool paired, bool authed, bool enc, bool keyed) {
  gLastHandler.clear();
  gHandlerCalls = 0;
  gCleanups = 0;
  gWarns = 0;
  gEspNow->encryptionEnabled = keyed;
  esp_now_recv_info ri{gSrc, nullptr, nullptr};
  EspNowV4Header h{};
  h.type = type;
  h.fragCount = 1;
  // A payload of the row's minimum length, so the policy gates are what decide.
  const V4OpcodeEntry* row = v4_dispatch_lookup(type);
  uint8_t payload[256] = {};
  const uint16_t len = row ? row->minLen : 16;
  bool claimed = v4_dispatch_claims(&ri, &h, payload, len, paired, authed, enc, "peer");
  return claimed && gHandlerCalls == 1;
}

// ---- TEXT receive path: dispatcher -> real v4h_text -> real drain -----------
// Locally administered test addresses: gPeerA is paired with registry name
// "alpha"; gStranger was never paired.
static uint8_t gPeerA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01};
static uint8_t gStranger[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x99};

static std::string macText(const uint8_t* mac) { return macToDisplayStr(mac).c_str(); }

static void resetText(bool keyed) {
  gState = EspNowState{};
  std::memcpy(gState.devices[0].mac, gPeerA, 6);
  gState.devices[0].name = String("alpha");
  gState.deviceCount = 1;
  gState.encryptionEnabled = keyed;
  gStored.clear();
  gStoreRefuses = false;
  gEvents.clear();
  gWarns = gPrints = gPushKicks = gCleanups = gHandlerCalls = 0;
  gLastHandler.clear();
  gBootRuleCalls = gBootSeenCalls = gHintRuleCalls = 0;
  gBootVerdict = 0;
  gFrameFp = kPeerAMeshFp;
}
static int queued() {
  return (gEspNow->textQueueHead - gEspNow->textQueueTail) & (EspNowState::TEXT_QUEUE_SIZE - 1);
}
static TextQueueEntry& queuedAt(int i) {
  return gEspNow->textQueue[(gEspNow->textQueueTail + i) & (EspNowState::TEXT_QUEUE_SIZE - 1)];
}

// Hands one already-verified frame to the dispatcher the way
// v4_try_handle_incoming does: `authed` = SESSION_FRAME unwrap or BROADCAST_AUTH
// verify succeeded, `enc` = SESSION_FRAME. A plaintext frame has both false;
// isPaired is the source-MAC lookup, so a spoofed paired MAC is "paired".
static bool deliver(uint8_t type, uint8_t* origin, uint8_t* radioSrc, const char* text,
                    bool authed, bool enc, uint8_t fragIndex = 0, uint8_t fragCount = 1,
                    uint32_t msgId = 1) {
  EspNowV4Header h{};
  h.type = type;
  h.headerLen = sizeof(EspNowV4Header);
  h.msgId = msgId;
  h.ttl = 1;
  h.fragIndex = fragIndex;
  h.fragCount = fragCount;
  h.meshFingerprint = gFrameFp;
  std::memcpy(h.origin, origin, 6);
  wifi_pkt_rx_ctrl_t ctrl{-50};
  esp_now_recv_info info{radioSrc, nullptr, &ctrl};
  const bool paired = std::memcmp(radioSrc, gPeerA, 6) == 0;
  const std::string name = paired ? std::string("alpha") : macText(radioSrc);
  return v4_dispatch_claims(&info, &h, reinterpret_cast<const uint8_t*>(text),
                               (uint16_t)std::strlen(text), paired, authed, enc, name.c_str());
}

static unsigned runTextCases() {
  unsigned failures = 0;
  auto check = [&](const char* what, bool ok) {
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  };

  // The spoof: keyed device, plaintext TEXT whose origin and radio source are
  // a paired peer's MAC (fingerprint 0, valid CRC16, no BROADCAST_AUTH).
  resetText(true);
  check("TEXT plaintext keyed from a paired MAC: claimed by the TEXT row",
        deliver(ESPNOW_V4_TYPE_TEXT, gPeerA, gPeerA, "hello", false, false));
  check("TEXT plaintext keyed from a paired MAC: dropped before v4h_text (nothing queued)", queued() == 0);
  check("TEXT plaintext keyed from a paired MAC: drop is logged (one WARN) and cleaned up",
        gWarns == 1 && gCleanups == 1);
  drainTextQueueForTest();
  check("TEXT plaintext keyed from a paired MAC: nothing stored in peer history", gStored.empty());
  check("TEXT plaintext keyed from a paired MAC: no SYSEVT_TEXT_RX posted", gEvents.empty());

  // BROADCAST_AUTH text (authenticated, not confidential) keeps everything.
  resetText(true);
  check("TEXT BROADCAST_AUTH keyed: claimed",
        deliver(ESPNOW_V4_TYPE_TEXT, gPeerA, gPeerA, "hi all", true, false));
  check("TEXT BROADCAST_AUTH keyed: queued, reported not encrypted",
        queued() == 1 && !queuedAt(0).encrypted);
  check("TEXT BROADCAST_AUTH keyed: queued under the registry name",
        std::strcmp(queuedAt(0).deviceName, "alpha") == 0);
  drainTextQueueForTest();
  check("TEXT BROADCAST_AUTH keyed: stored as chat under the registry name",
        gStored.size() == 1 && gStored[0].name == "alpha" && !gStored[0].encrypted &&
        gStored[0].type == MSG_TEXT);
  check("TEXT BROADCAST_AUTH keyed: stored as Attributed",
        gStored.size() == 1 && gStored[0].attribution == HistoryAttribution::Attributed);
  check("TEXT BROADCAST_AUTH keyed: posts SYSEVT_TEXT_RX(subject=name, detail=text)",
        gEvents.size() == 1 && gEvents[0].kind == SYSEVT_TEXT_RX && gEvents[0].subject == "alpha" &&
        gEvents[0].detail == "hi all");
  check("TEXT BROADCAST_AUTH keyed: G2 push-kick and console line once each",
        gPushKicks == 1 && gPrints == 1);

  // Session-encrypted unicast text.
  resetText(true);
  check("TEXT session-encrypted keyed: claimed",
        deliver(ESPNOW_V4_TYPE_TEXT, gPeerA, gPeerA, "secret", true, true));
  check("TEXT session-encrypted keyed: queued, reported encrypted", queued() == 1 && queuedAt(0).encrypted);
  drainTextQueueForTest();
  check("TEXT session-encrypted keyed: stored encrypted and posts SYSEVT_TEXT_RX",
        gStored.size() == 1 && gStored[0].encrypted && gEvents.size() == 1);

  // The history refuses the record (full, or no memory for the peer's block):
  // the message still arrived, so text_rx still fires; there is nothing new to
  // draw, so no push-kick.
  resetText(true);
  check("TEXT session-encrypted, history refuses: claimed",
        deliver(ESPNOW_V4_TYPE_TEXT, gPeerA, gPeerA, "kept?", true, true));
  gStoreRefuses = true;
  drainTextQueueForTest();
  check("TEXT session-encrypted, history refuses: SYSEVT_TEXT_RX still posted",
        gStored.empty() && gEvents.size() == 1 && gEvents[0].kind == SYSEVT_TEXT_RX && gEvents[0].detail == "kept?");
  check("TEXT session-encrypted, history refuses: no G2 push-kick, console line still printed",
        gPushKicks == 0 && gPrints == 1);

  // Keyless device (bootstrap): plaintext is still accepted, but a spoofed
  // paired MAC never borrows the paired name and never reaches the event bus.
  resetText(false);
  check("TEXT plaintext keyless from a paired MAC: claimed",
        deliver(ESPNOW_V4_TYPE_TEXT, gPeerA, gPeerA, "spoof", false, false));
  check("TEXT plaintext keyless from a paired MAC: accepted without a WARN (bootstrap compatibility)",
        queued() == 1 && gWarns == 0);
  check("TEXT plaintext keyless from a paired MAC: not attributed to the paired name",
        std::strcmp(queuedAt(0).deviceName, "alpha") != 0);
  check("TEXT plaintext keyless from a paired MAC: attributed to the origin MAC string",
        macText(gPeerA) == queuedAt(0).deviceName);
  check("TEXT plaintext keyless from a paired MAC: reported not encrypted", !queuedAt(0).encrypted);
  drainTextQueueForTest();
  check("TEXT plaintext keyless from a paired MAC: stored as chat under the MAC string",
        gStored.size() == 1 && gStored[0].name == macText(gPeerA) && gStored[0].mac == macText(gPeerA) &&
        gStored[0].type == MSG_TEXT);
  check("TEXT plaintext keyless from a paired MAC: stored as Unattributed (shown under the MAC text)",
        gStored.size() == 1 && gStored[0].attribution == HistoryAttribution::Unattributed);
  check("TEXT plaintext keyless from a paired MAC: no SYSEVT_TEXT_RX posted", gEvents.empty());
  check("TEXT plaintext keyless from a paired MAC: G2 push-kick and console line still happen",
        gPushKicks == 1 && gPrints == 1);

  // Keyless device, plaintext from a stranger.
  resetText(false);
  check("TEXT plaintext keyless from a stranger: claimed",
        deliver(ESPNOW_V4_TYPE_TEXT, gStranger, gStranger, "yo", false, false));
  check("TEXT plaintext keyless from a stranger: queued under its MAC string",
        queued() == 1 && macText(gStranger) == queuedAt(0).deviceName);
  drainTextQueueForTest();
  check("TEXT plaintext keyless from a stranger: stored, no SYSEVT_TEXT_RX posted",
        gStored.size() == 1 && gEvents.empty());

  // BOOT (31) was retired in ESP-NOW 1.0 step 8: the heartbeat's boot counter
  // carries a restart. No row, so a frame of it is refused as unknown.
  check("opcode 31 (BOOT, retired in 1.0) has no row", v4_dispatch_lookup(31) == nullptr);

  // SESSION_UNKNOWN (when the tree has the row): only from the peer itself,
  // under its own mesh's tag.
  if (v4_dispatch_lookup(18) != nullptr) {
    static const uint8_t hint[12] = {0x3F, 0x40, 0, 0, 1, 0, 0, 0, 0x2A, 0, 0, 0};
    const char* hintText = reinterpret_cast<const char*>(hint);
    auto deliverHint = [&](uint8_t* origin, uint8_t* radioSrc, bool authed, bool enc) {
      EspNowV4Header h{};
      h.type = 18;
      h.headerLen = sizeof(EspNowV4Header);
      h.msgId = 2;
      h.ttl = 1;
      h.fragCount = 1;
      h.meshFingerprint = gFrameFp;
      std::memcpy(h.origin, origin, 6);
      wifi_pkt_rx_ctrl_t ctrl{-50};
      esp_now_recv_info info{radioSrc, nullptr, &ctrl};
      const bool paired = std::memcmp(radioSrc, gPeerA, 6) == 0;
      v4_dispatch_claims(&info, &h, reinterpret_cast<const uint8_t*>(hintText), sizeof hint, paired,
                            authed, enc, "alpha");
    };
    resetText(true);
    deliverHint(gPeerA, gPeerA, true, false);
    check("SESSION_UNKNOWN group-signed from the peer itself: the hint rule asked once", gHintRuleCalls == 1);
    resetText(true);
    deliverHint(gPeerA, gPeerA, false, false);
    check("SESSION_UNKNOWN plaintext: the hint rule is not asked", gHintRuleCalls == 0);
    resetText(true);
    deliverHint(gPeerA, gStranger, true, false);
    check("SESSION_UNKNOWN from an unpaired radio sender: the hint rule is not asked", gHintRuleCalls == 0);
    resetText(true);
    gFrameFp = fingerprintForPeer(gStranger);  // a tag the origin's mesh would carry
    deliverHint(gStranger, gPeerA, true, false);
    check("SESSION_UNKNOWN whose origin is not the radio sender: the hint rule is not asked",
          gHintRuleCalls == 0);
    resetText(true);
    gFrameFp = kOtherMeshFp;
    deliverHint(gPeerA, gPeerA, true, false);
    check("SESSION_UNKNOWN under another mesh's tag: the hint rule is not asked "
          "(FAILS ON the unscoped handler)", gHintRuleCalls == 0);
  }

  // Relayed text: an unknown origin is dropped by espnowOriginIsKnown, a known
  // origin is attributed to that origin.
  resetText(false);
  check("TEXT relayed from an unknown origin: claimed",
        deliver(ESPNOW_V4_TYPE_TEXT, gStranger, gPeerA, "relayed", false, false));
  check("TEXT relayed from an unknown origin: dropped by espnowOriginIsKnown without a WARN",
        queued() == 0 && gWarns == 0);
  resetText(true);
  check("TEXT relayed authenticated from a known origin: claimed",
        deliver(ESPNOW_V4_TYPE_TEXT, gPeerA, gStranger, "relayed", true, false));
  check("TEXT relayed authenticated from a known origin: attributed to the origin's registry name",
        queued() == 1 && std::strcmp(queuedAt(0).deviceName, "alpha") == 0);

  // Authenticated multi-fragment text: one record per piece, one bus event.
  resetText(true);
  deliver(ESPNOW_V4_TYPE_TEXT, gPeerA, gPeerA, "part one ", true, true, 0, 2, 77);
  deliver(ESPNOW_V4_TYPE_TEXT, gPeerA, gPeerA, "part two", true, true, 1, 2, 77);
  check("TEXT two session-encrypted fragments: two queue entries", queued() == 2);
  drainTextQueueForTest();
  check("TEXT two session-encrypted fragments: stored as pieces 1/2 and 2/2 sharing the msgId",
        gStored.size() == 2 && gStored[0].piece == 1 && gStored[0].of == 2 && gStored[1].piece == 2 &&
        gStored[1].of == 2 && gStored[0].reqId == 77 && gStored[1].reqId == 77);
  check("TEXT two session-encrypted fragments: one SYSEVT_TEXT_RX carrying the last fragment",
        gEvents.size() == 1 && gEvents[0].detail == "part two");
  check("TEXT two session-encrypted fragments: drain consumes the queue", queued() == 0);

  // Table policy for the chat row (BOOT, which shared it, was retired in 1.0
  // step 8). The flag's own name is not used so the harness also compiles
  // against a table that predates it.
  const V4OpcodeEntry* textRow = v4_dispatch_lookup(ESPNOW_V4_TYPE_TEXT);
  check("TEXT row carries a receive policy (non-zero flags)", textRow && textRow->flags != 0);
  check("TEXT row has no unconditional REQ_PAIRED/REQ_AUTHENTICATED/REQ_SESSION_ENC (keyless bootstrap)",
        textRow && (textRow->flags & (V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_AUTHENTICATED |
                                      V4_OPC_FLAG_REQ_SESSION_ENC)) == 0);
  return failures;
}

// ---- Bond rows: only the configured bond partner reaches a bond handler ------
// Every opcode in the table's ENABLE_BONDED_MODE block (the runner checks the
// list against the table).
static const uint8_t kBondOpcodes[] = {
// INSERT_BOND_OPCODES
};
// The bond agreement rows (the table's second ENABLE_BONDED_MODE block):
// BOND_REQUEST and BOND_NOTICE, from any paired peer over its session. Empty
// for a source from before them.
static const std::vector<uint8_t> kBondAgreementOpcodes = {
// INSERT_BOND_AGREEMENT_OPCODES
};
// Locally administered test addresses: gPartner is the configured bond partner,
// gOtherPaired is a second paired mesh peer with a live session.
static uint8_t gPartner[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x02};
static uint8_t gOtherPaired[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x03};

// Dispatches one bond frame from `src`; true when exactly one handler ran.
// `minuteLater` moves the clock past the gate's one-line-a-minute limit first.
static bool bondRun(uint8_t type, uint8_t* src, bool paired, bool enc, bool minuteLater = true) {
  if (minuteLater) gNowMs += 61000;
  gLastHandler.clear();
  gHandlerCalls = gCleanups = gWarns = gParseCalls = gIgnoredNotes = 0;
  esp_now_recv_info ri{src, nullptr, nullptr};
  EspNowV4Header h{};
  h.type = type;
  h.fragCount = 1;
  const V4OpcodeEntry* row = v4_dispatch_lookup(type);
  uint8_t payload[256] = {};
  const uint16_t len = row ? row->minLen : 16;
  const bool claimed = v4_dispatch_claims(&ri, &h, payload, len, paired, enc, enc, "peer");
  return claimed && gHandlerCalls == 1;
}

static unsigned runBondCases() {
  unsigned failures = 0;
  auto check = [&](const std::string& what, bool ok) {
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  };
  gEspNow->encryptionEnabled = true;
#if ENABLE_BONDED_MODE
  auto expect = [&](const std::string& what, bool got, bool want) {
    if (got != want) ++failures;
    if (got != want) std::printf("FAIL %s: got %s, want %s\n", what.c_str(),
                                 got ? "dispatched" : "dropped", want ? "dispatched" : "dropped");
    else std::printf("ok   %s\n", what.c_str());
  };
  const std::string partnerText = macText(gPartner);
  for (uint8_t op : kBondOpcodes) {
    const std::string name = opcodeName(op);
    const V4OpcodeEntry* row = v4_dispatch_lookup(op);
    check(name + " row requires REQ_PAIRED|REQ_SESSION_ENC",
          row && (row->flags & (V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC)) ==
                     (V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC));

    gSettings.bondModeEnabled = true;
    gSettings.bondPeerMac = String(partnerText.c_str());
    expect(name + " from the bond partner, session-encrypted", bondRun(op, gPartner, true, true), true);
    check(name + " from the bond partner: no WARN, cleaned up once", gWarns == 0 && gCleanups == 1);
    expect(name + " from a second paired peer, session-encrypted (bond mode on)",
           bondRun(op, gOtherPaired, true, true), false);
    check(name + " from a second paired peer: drop is logged (one WARN) and cleaned up",
          gWarns == 1 && gCleanups == 1 && gHandlerCalls == 0);
    check(name + " from a second paired peer over its session is answered kRefused (1.0 step 5b, G3)",
          gLastVerdict.kind == V4VerdictKind::Refused && gLastVerdict.status == espnowanswer::kRefused);
    bondRun(op, gOtherPaired, true, false);
    check(name + " from a second paired peer, not sealed: silence (no session peer, G3)",
          gLastVerdict.kind == V4VerdictKind::Silent);
    expect(name + " from the bond partner, plaintext", bondRun(op, gPartner, true, false), false);
    expect(name + " from an unpaired source, session-encrypted", bondRun(op, gOtherPaired, false, true), false);

    gSettings.bondPeerMac = String("");
    expect(name + " with bond mode on and no partner configured", bondRun(op, gPartner, true, true), false);

    gSettings.bondModeEnabled = false;
    gSettings.bondPeerMac = String(partnerText.c_str());
    expect(name + " from the bond partner with bond mode off", bondRun(op, gPartner, true, true), false);
    check(name + " with bond mode off: dropped without a WARN, cleaned up", gWarns == 0 && gCleanups == 1);
    expect(name + " from a second paired peer with bond mode off", bondRun(op, gOtherPaired, true, true), false);
  }

  // How the partner is stored. bondconnect writes the DISPLAY form; both answers
  // for it are a text compare (parseMacAddress, which allocates, is not called).
  const uint8_t hb = ESPNOW_V4_TYPE_BOND_HEARTBEAT;
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = String(partnerText.c_str());
  expect("partner stored in DISPLAY form: partner accepted", bondRun(hb, gPartner, true, true), true);
  check("partner stored in DISPLAY form: partner accepted without parseMacAddress", gParseCalls == 0);
  expect("partner stored in DISPLAY form: second paired peer dropped", bondRun(hb, gOtherPaired, true, true), false);
  check("partner stored in DISPLAY form: second paired peer dropped without parseMacAddress", gParseCalls == 0);
  // The setting can be typed by hand (espnowbondpeermac): the partner the
  // senders resolve with parseMacAddress is the partner the gate accepts.
  gSettings.bondPeerMac = String("02-11-22-33-44-02");
  expect("partner typed with dashes: partner accepted", bondRun(hb, gPartner, true, true), true);
  check("partner typed with dashes: resolved by parseMacAddress, the senders' parser", gParseCalls == 1);
  expect("partner typed with dashes: second paired peer dropped", bondRun(hb, gOtherPaired, true, true), false);
  gSettings.bondPeerMac = String("02:11:22:33:44:0a");
  uint8_t lowerCasePartner[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x0A};
  expect("partner typed in lower case: partner accepted", bondRun(hb, lowerCasePartner, true, true), true);
  expect("partner typed in lower case: second paired peer dropped", bondRun(hb, gOtherPaired, true, true), false);
  gSettings.bondPeerMac = String("not a mac");
  expect("partner text that does not parse: dropped", bondRun(hb, gPartner, true, true), false);

  // A peer whose own partner setting is stale sends a heartbeat every few
  // seconds: one WARN a minute, every frame still dropped and cleaned up.
  gSettings.bondPeerMac = String(partnerText.c_str());
  expect("stale peer, first heartbeat", bondRun(hb, gOtherPaired, true, true), false);
  check("stale peer, first heartbeat: one WARN", gWarns == 1);
  gNowMs += 5000;
  expect("stale peer, heartbeat 5 s later", bondRun(hb, gOtherPaired, true, true, false), false);
  check("stale peer, heartbeat 5 s later: dropped and cleaned up without a second WARN",
        gWarns == 0 && gCleanups == 1 && gHandlerCalls == 0);
  gNowMs += 56000;
  expect("stale peer, heartbeat a minute after the first", bondRun(hb, gOtherPaired, true, true, false), false);
  check("stale peer, heartbeat a minute after the first: WARN again", gWarns == 1);
  expect("partner heartbeat between the stale ones is still accepted",
         bondRun(hb, gPartner, true, true, false), true);

  // Bond agreement rows: the sender may not be the partner (saying so is what
  // BOND_NOTICE is for), so there is no REQ_BOND_PEER; a paired peer's session
  // is still required.
  for (uint8_t op : kBondAgreementOpcodes) {
    const std::string name = opcodeName(op);
    const V4OpcodeEntry* row = v4_dispatch_lookup(op);
    check(name + " row is REQ_PAIRED|REQ_SESSION_ENC without REQ_BOND_PEER",
          row && row->flags == (V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC));
    gSettings.bondModeEnabled = true;
    gSettings.bondPeerMac = String(partnerText.c_str());
    expect(name + " from the bond partner, session-encrypted", bondRun(op, gPartner, true, true), true);
    expect(name + " from a second paired peer, session-encrypted (bond mode on)",
           bondRun(op, gOtherPaired, true, true), true);
    check(name + " from a second paired peer: no WARN, cleaned up once", gWarns == 0 && gCleanups == 1);
    expect(name + " from a paired peer, plaintext", bondRun(op, gOtherPaired, true, false), false);
    check(name + " plaintext: drop is logged (one WARN) and cleaned up", gWarns == 1 && gCleanups == 1);
    expect(name + " from an unpaired source, session-encrypted", bondRun(op, gOtherPaired, false, true), false);
    gSettings.bondModeEnabled = false;
    gSettings.bondPeerMac = String("");
    expect(name + " from a paired peer with bond mode off", bondRun(op, gOtherPaired, true, true), true);
  }
  if (!kBondAgreementOpcodes.empty()) {
    // The drop path tells the agreement whose bond frame was ignored, so that
    // peer can be sent a notice: a paired peer that is not the partner, with
    // bond mode on or off. Not the partner, not an unpaired source.
    gSettings.bondModeEnabled = true;
    gSettings.bondPeerMac = String(partnerText.c_str());
    bondRun(hb, gOtherPaired, true, true);
    check("an ignored bond frame from a second paired peer is noted for a notice",
          gIgnoredNotes == 1 && std::memcmp(gIgnoredFrom, gOtherPaired, 6) == 0 && gIgnoredOverSession);
    bondRun(hb, gPartner, true, true);
    check("the partner's bond frame is not noted", gIgnoredNotes == 0);
    bondRun(hb, gOtherPaired, false, true);
    check("an unpaired source's bond frame is not noted", gIgnoredNotes == 0);
    bondRun(hb, gOtherPaired, true, false);
    check("a plaintext bond frame is noted as not from a session (no notice follows)",
          gIgnoredNotes == 1 && !gIgnoredOverSession);
    gSettings.bondModeEnabled = false;
    bondRun(hb, gOtherPaired, true, true);
    check("with bond mode off an ignored bond frame is noted too",
          gIgnoredNotes == 1 && std::memcmp(gIgnoredFrom, gOtherPaired, 6) == 0 && gIgnoredOverSession);
    for (uint8_t op : kBondAgreementOpcodes) {
      bondRun(op, gOtherPaired, true, true);
      check(std::string(opcodeName(op)) + " itself is never noted as ignored", gIgnoredNotes == 0);
    }
  }
#else
  // Bonded mode compiled out: no bond row exists, so the frames fall to the
  // unknown-type drop whatever the settings say.
  for (uint8_t op : kBondOpcodes) {
    check(std::string(opcodeName(op)) + " has no table row (bonded mode compiled out)",
          v4_dispatch_lookup(op) == nullptr && !bondRun(op, gPartner, true, true) &&
              !bondRun(op, gOtherPaired, true, true));
  }
  for (uint8_t op : kBondAgreementOpcodes) {
    check(std::string(opcodeName(op)) + " has no table row (bonded mode compiled out)",
          v4_dispatch_lookup(op) == nullptr && !bondRun(op, gPartner, true, true) &&
              !bondRun(op, gOtherPaired, true, true));
  }
#endif
  return failures;
}

// ---- The length column: every row's minimum, enforced before the handler -----
static const struct { uint8_t opcode; uint16_t minLen; bool exact; } kExpectedMinLen[] = {
// INSERT_EXPECTED_MINLEN
};

// One frame of `len` bytes for `row`, with every trust bit its policy gates ask
// for (paired, sealed, from the configured bond partner on a keyed device).
static bool lengthRun(const V4OpcodeEntry& row, uint16_t len) {
  gHandlerCalls = 0;
  gLastHandler.clear();
  gLastPayloadLen = 0;
  gEspNow->encryptionEnabled = true;
  esp_now_recv_info ri{gSrc, nullptr, nullptr};
  EspNowV4Header h{};
  h.type = row.opcode;
  h.fragCount = 1;
  static uint8_t payload[300];
  std::memset(payload, 0x20, sizeof payload);  // printable, so the text row takes it
  return v4_dispatch_claims(&ri, &h, payload, len, true, true, true, "peer");
}

static unsigned runLengthCases() {
  unsigned failures = 0;
  auto check = [&](const std::string& what, bool ok) {
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  };
  check("the length column: one expected minimum per row (minLen never rises after 1.0)",
        sizeof(kExpectedMinLen) / sizeof(kExpectedMinLen[0]) == kV4HandlerTableSize);
  gSettings.bondModeEnabled = true;
  gSettings.bondPeerMac = String(macText(gSrc).c_str());
  gNowMs += 61000;
  for (const auto& want : kExpectedMinLen) {
    const V4OpcodeEntry* row = v4_dispatch_lookup(want.opcode);
    const std::string name = opcodeName(want.opcode);
    check(name + ": minLen " + std::to_string(want.minLen) + (want.exact ? " (exact)" : "") +
              " (minLen never rises after 1.0)",
          row && row->minLen == want.minLen && ((row->flags & V4_OPC_LEN_EXACT) != 0) == want.exact);
    if (!row) continue;
    if (want.minLen > 0) {
      const uint32_t before = gEspNowRxLenDrops;
      const bool claimed = lengthRun(*row, (uint16_t)(want.minLen - 1));
      check(name + ": one byte short is claimed, dropped before the handler and counted once",
            claimed && gHandlerCalls == 0 && gEspNowRxLenDrops == before + 1);
    }
    {
      const uint32_t before = gEspNowRxLenDrops;
      const bool claimed = lengthRun(*row, want.minLen);
      const bool realHandler = gLastHandler.empty();
      check(name + ": exactly the minimum passes the length gate",
            claimed && gEspNowRxLenDrops == before && (realHandler || (gHandlerCalls == 1 &&
                                                                          gLastPayloadLen == want.minLen)));
    }
    {
      const uint32_t before = gEspNowRxLenDrops;
      const uint16_t longer = (uint16_t)(want.minLen + (want.exact ? 1 : 7));
      const bool claimed = lengthRun(*row, longer);
      if (want.exact) {
        check(name + ": a handshake frame one byte long is dropped and counted (exact length)",
              claimed && gHandlerCalls == 0 && gEspNowRxLenDrops == before + 1);
      } else {
        const bool realHandler = gLastHandler.empty();
        check(name + ": a longer frame reaches the handler with its length intact (read up to known fields)",
              claimed && gEspNowRxLenDrops == before &&
                  (realHandler || (gHandlerCalls == 1 && gLastPayloadLen == longer)));
      }
    }
  }
  gSettings.bondModeEnabled = false;
  gSettings.bondPeerMac = String("");

  // A 0-byte STREAM is a length drop: the empty STREAM_BEGIN / STREAM_END
  // markers of a remote command are gone in 1.0, and sendEspNowStreamMessage
  // skips a blank line (espnow_tx_multiframe S3b), so no sender sends one.
  {
    const V4OpcodeEntry* row = v4_dispatch_lookup(ESPNOW_V4_TYPE_STREAM);
    const uint32_t before = gEspNowRxLenDrops;
    check("an empty STREAM is dropped and counted (no 1.0 sender sends one)",
          row && lengthRun(*row, 0) && gEspNowRxLenDrops == before + 1 && gHandlerCalls == 0);
  }
  // SESSION_UNKNOWN from a later sender (20 bytes): the hint still reaches its rule.
  {
    resetText(true);
    EspNowV4Header h{};
    h.type = ESPNOW_V4_TYPE_SESSION_UNKNOWN;
    h.headerLen = sizeof(EspNowV4Header);
    h.fragCount = 1;
    h.ttl = 1;
    h.meshFingerprint = kPeerAMeshFp;
    std::memcpy(h.origin, gPeerA, 6);
    wifi_pkt_rx_ctrl_t ctrl{-50};
    esp_now_recv_info info{gPeerA, nullptr, &ctrl};
    uint8_t payload[20] = {0x2A, 0x00};
    const uint32_t before = gEspNowRxLenDrops;
    const bool claimed = v4_dispatch_claims(&info, &h, payload, sizeof payload, true, true, false, "alpha");
    check("SESSION_UNKNOWN of 20 bytes (a later sender) reaches the SESSION_UNKNOWN rule",
          claimed && gHintRuleCalls == 1 && gEspNowRxLenDrops == before);
  }
  return failures;
}

// The table walk (1.0 step 5b): every row answers with a verdict. With every
// gate passed and the row's minimum length the (stub or real) handler decides;
// one byte short is kBadRequest; a gate that wants a paired sender or a
// session says nothing to a sender that is neither (silence); an opcode
// without a row is kUnsupported.
static unsigned runVerdictWalk() {
  unsigned failures = 0;
  auto check = [&](const std::string& what, bool ok) {
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  };
  gEspNow->encryptionEnabled = true;
  unsigned rows = 0, stubAccepted = 0, shortBad = 0, gateSilent = 0, gated = 0, pairedRows = 0, pairedRefused = 0;
  for (size_t i = 0; i < kV4HandlerTableSize; i++) {
    const V4OpcodeEntry& row = kV4HandlerTable[i];
    if (row.flags & V4_OPC_FLAG_REQ_BOND_PEER) continue;  // runBondCases walks the bond rows
    ++rows;
    const std::string name = opcodeName(row.opcode);
    esp_now_recv_info ri{gSrc, nullptr, nullptr};
    EspNowV4Header h{};
    h.type = row.opcode;
    h.fragCount = 1;
    uint8_t payload[256] = {};
    gHandlerCalls = 0;
    gLastHandler.clear();
    V4Verdict v = v4_dispatch_table_try(&ri, &h, payload, row.minLen, true, true, true, "peer");
    if (gHandlerCalls == 1 && v.kind == V4VerdictKind::Accepted) ++stubAccepted;
    else if (gHandlerCalls == 1) check(name + ": the handler's own verdict is returned", true);
    if (row.minLen > 0) {
      gHandlerCalls = 0;
      v = v4_dispatch_table_try(&ri, &h, payload, (uint16_t)(row.minLen - 1), true, true, true, "peer");
      if (gHandlerCalls == 0 && v.kind == V4VerdictKind::Refused && v.status == espnowanswer::kBadRequest) ++shortBad;
      else check(name + " one byte short is answered kBadRequest", false);
    } else {
      ++shortBad;
    }
    if (row.flags & (V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC)) {
      ++gated;
      const bool paired = !(row.flags & V4_OPC_FLAG_REQ_PAIRED);
      gHandlerCalls = 0;
      v = v4_dispatch_table_try(&ri, &h, payload, row.minLen, paired, true, false, "peer");
      if (gHandlerCalls == 0 && v.kind == V4VerdictKind::Silent) ++gateSilent;
      else check(name + " refused at a gate to a sender without a session: silence", false);
    }
    if (row.flags & V4_OPC_FLAG_REQ_PAIRED) {
      ++pairedRows;
      gHandlerCalls = 0;
      v = v4_dispatch_table_try(&ri, &h, payload, row.minLen, false, true, true, "peer");
      if (gHandlerCalls == 0 && v.kind == V4VerdictKind::Refused && v.status == espnowanswer::kRefused) ++pairedRefused;
      else check(name + " from a sealed session peer that is not paired: kRefused", false);
    }
  }
  check("every non-bond row with its gates passed reaches its handler and returns its verdict (" +
            std::to_string(stubAccepted) + " stub rows accepted of " + std::to_string(rows) + ")",
        stubAccepted + 3 >= rows && stubAccepted > 0);
  check("every row one byte short is answered kBadRequest (" + std::to_string(shortBad) + "/" +
            std::to_string(rows) + ")", shortBad == rows);
  check("every gated row refused to an unpaired or unsealed sender is silent (" + std::to_string(gateSilent) +
            "/" + std::to_string(gated) + ")", gateSilent == gated && gated > 0);
  check("every REQ_PAIRED row refuses a sealed frame from an unpaired session peer with kRefused (" +
            std::to_string(pairedRefused) + "/" + std::to_string(pairedRows) + ")",
        pairedRefused == pairedRows && pairedRows > 0);
  for (unsigned op = 0; op < 256; ++op) {
    if (v4_dispatch_lookup((uint8_t)op)) continue;
    esp_now_recv_info ri{gSrc, nullptr, nullptr};
    EspNowV4Header h{};
    h.type = (uint8_t)op;
    h.fragCount = 1;
    uint8_t payload[16] = {};
    const V4Verdict v = v4_dispatch_table_try(&ri, &h, payload, sizeof payload, true, true, true, "peer");
    if (!(v.kind == V4VerdictKind::Refused && v.status == espnowanswer::kUnsupported)) {
      check("opcode " + std::to_string(op) + " without a row is answered kUnsupported", false);
    }
  }
  check("every opcode without a row is answered kUnsupported", true);
  // TEXT into a full ring: not taken, answered busy (the sender no longer
  // shows Delivered), and the ring is unchanged.
  {
    esp_now_recv_info ri{gSrc, nullptr, nullptr};
    EspNowV4Header h{};
    h.type = ESPNOW_V4_TYPE_TEXT;
    h.fragCount = 1;
    std::memcpy(h.origin, gSrc, 6);
    const uint8_t text[] = "hello";
    gEspNow->textQueueTail = 0;
    gEspNow->textQueueHead = EspNowState::TEXT_QUEUE_SIZE - 1;  // one short of the tail: full
    const V4Verdict v = v4_dispatch_table_try(&ri, &h, text, 5, true, true, true, "peer");
    check("TEXT into a full ring is refused busy and the ring is unchanged",
          v.kind == V4VerdictKind::Refused && v.status == espnowanswer::kBusy &&
              gEspNow->textQueueHead == EspNowState::TEXT_QUEUE_SIZE - 1);
    gEspNow->textQueueHead = gEspNow->textQueueTail = 0;
  }
  return failures;
}

int main() {
  unsigned failures = 0;
  auto expect = [&](const char* what, bool got, bool want) {
    if (got != want) {
      ++failures;
      std::printf("FAIL %s: got %s, want %s\n", what,
                  got ? "dispatched" : "dropped", want ? "dispatched" : "dropped");
    } else {
      std::printf("ok   %s\n", what);
    }
  };
  auto check = [&](const char* what, bool ok) {
    if (!ok) {
      ++failures;
      std::printf("FAIL %s\n", what);
    } else {
      std::printf("ok   %s\n", what);
    }
  };

  // Mesh time and the sensor frames: TIME_SYNC can set the clock, SENSOR_STATUS
  // and SENSOR_ENVELOPE make and fill entries of the receiver's table,
  // SENSOR_REQ starts a stream, POWER_STATUS and EVENT_PUSH fill the master's
  // battery and event stores. Each needs a paired sender and a
  // session-encrypted frame, on a keyed and on a keyless device; a signed
  // broadcast is not enough (TIME_SYNC and SENSOR_STATUS were one). None is a
  // bond frame: a paired peer that is not the bond partner reaches them.
  // FORWARD_LINK (a bond master announcing to a mesh master that it forwards
  // its partner's data) is the same: the master is not its bond partner, and
  // a master built without bond mode must take it.
  const uint8_t kPairedMeshRows[] = { ESPNOW_V4_TYPE_TIME_SYNC, ESPNOW_V4_TYPE_SENSOR_STATUS,
                                      ESPNOW_V4_TYPE_SENSOR_REQ, ESPNOW_V4_TYPE_SENSOR_ENVELOPE,
                                      ESPNOW_V4_TYPE_POWER_STATUS, ESPNOW_V4_TYPE_EVENT_PUSH,
                                      ESPNOW_V4_TYPE_FORWARD_LINK };
  for (uint8_t op : kPairedMeshRows) {
    const std::string name = opcodeName(op);
    const V4OpcodeEntry* row = v4_dispatch_lookup(op);
    check((name + " row is REQ_PAIRED|REQ_SESSION_ENC, nothing else (not bond-gated)").c_str(),
          row && row->flags == (V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC));
    for (bool keyed : { true, false }) {
      const std::string on = keyed ? " keyed" : " unkeyed";
      expect((name + " paired plaintext" + on).c_str(), run(op, true, false, false, keyed), false);
      check((name + " paired plaintext" + on + ": drop is logged (one WARN) and cleaned up").c_str(),
            gWarns == 1 && gCleanups == 1);
      expect((name + " paired BROADCAST_AUTH-only" + on).c_str(), run(op, true, true, false, keyed), false);
      expect((name + " unpaired BROADCAST_AUTH-only" + on).c_str(), run(op, false, true, false, keyed), false);
      expect((name + " unpaired session-encrypted" + on).c_str(), run(op, false, true, true, keyed), false);
      expect((name + " paired session-encrypted" + on).c_str(), run(op, true, true, true, keyed), true);
    }
#if ENABLE_BONDED_MODE
    // Bond mode on with another device as partner: the sender (gSrc) is a
    // paired peer that is not the partner, and still reaches the handler.
    gSettings.bondModeEnabled = true;
    gSettings.bondPeerMac = String("02:AA:AA:AA:AA:01");
    expect((name + " from a paired peer that is not the bond partner (bond mode on)").c_str(),
           run(op, true, true, true, true), true);
    gSettings.bondModeEnabled = false;
    gSettings.bondPeerMac = String("");
#endif
  }

  // HEARTBEAT records liveness that routing reads: same policy. Every sender
  // tags (periodic broadcast) or seals (unicast) it once a mesh key exists.
  expect("HEARTBEAT plaintext keyed from a paired MAC",
         run(ESPNOW_V4_TYPE_HEARTBEAT, true, false, false, true), false);
  check("HEARTBEAT plaintext keyed drop is logged (one WARN) and cleaned up",
        gWarns == 1 && gCleanups == 1);
  expect("HEARTBEAT BROADCAST_AUTH keyed from an unpaired neighbour",
         run(ESPNOW_V4_TYPE_HEARTBEAT, false, true, false, true), true);
  expect("HEARTBEAT session-encrypted keyed",
         run(ESPNOW_V4_TYPE_HEARTBEAT, true, true, true, true), true);
  expect("HEARTBEAT plaintext unkeyed (bootstrap compatibility)",
         run(ESPNOW_V4_TYPE_HEARTBEAT, true, false, false, false), true);

  // The retired plaintext SENSOR_BROADCAST (150) has no row: the frame falls
  // to the unknown-type drop. The literal is used because the enum symbol is gone.
  check("opcode 150 (retired SENSOR_BROADCAST) has no table row",
        v4_dispatch_lookup(150) == nullptr);

  // Table policy as written.
  // 130 (SUBSCRIBE_UPDATE) was on the wire before 1.0 and is retired: no row.
  check("opcode 130 (retired SUBSCRIBE_UPDATE) has no table row", v4_dispatch_lookup(130) == nullptr);
  const V4OpcodeEntry* e = nullptr;
  // USER_SYNC carries account records: the table refuses a frame not sealed in a
  // session before the handler (which checks again) runs.
  e = v4_dispatch_lookup(ESPNOW_V4_TYPE_USER_SYNC);
  check("USER_SYNC row requires REQ_SESSION_ENC", e && (e->flags & V4_OPC_FLAG_REQ_SESSION_ENC) != 0);
  check("plaintext USER_SYNC from a paired peer reaches no handler",
        !run(ESPNOW_V4_TYPE_USER_SYNC, /*paired=*/true, /*authed=*/false, /*enc=*/false, /*keyed=*/true));
  check("group-signed USER_SYNC reaches no handler",
        !run(ESPNOW_V4_TYPE_USER_SYNC, /*paired=*/true, /*authed=*/true, /*enc=*/false, /*keyed=*/true));
  check("session-sealed USER_SYNC reaches its handler",
        run(ESPNOW_V4_TYPE_USER_SYNC, /*paired=*/true, /*authed=*/true, /*enc=*/true, /*keyed=*/true));

  // Verifier's table-driven invariant: no state-mutating opcode has empty flags.
  bool zeroFlagOk = true;
  for (size_t i = 0; i < kV4HandlerTableSize; i++) {
    const V4OpcodeEntry& row = kV4HandlerTable[i];
    if ((row.flags & ~V4_OPC_LEN_EXACT) != 0 || zeroFlagAllowed(row.opcode)) continue;
    zeroFlagOk = false;
    std::printf("FAIL zero-flag row outside the allow-list: opcode %u (%s) accepts plaintext from any MAC\n",
                (unsigned)row.opcode, opcodeName(row.opcode));
    ++failures;
  }
  if (zeroFlagOk) std::printf("ok   every zero-flag row is on the allow-list\n");

  failures += runTextCases();
  failures += runBondCases();
  failures += runLengthCases();
  failures += runVerdictWalk();

  std::printf("ENABLE_BONDED_MODE=%d: %u failure%s\n", ENABLE_BONDED_MODE, failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
