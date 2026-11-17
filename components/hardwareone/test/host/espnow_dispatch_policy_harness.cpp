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

static void noteHandler(const char* name, const V4RxCtx& ctx) {
  (void)ctx;
  gLastHandler = name;
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
static void noteMeshPeerRxActivity(const uint8_t*, EspNowMeshRxKind, int8_t, int8_t) {}
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
struct StoredMessage { std::string mac, name, content; bool encrypted; int type; uint32_t reqId; uint8_t piece, of; };
static std::vector<StoredMessage> gStored;
static bool storeMessageInPeerHistory(uint8_t* mac, const char* name, const char* message, bool encrypted,
                                      LogMessageType type, uint32_t reqId = 0, uint8_t piece = 1,
                                      uint8_t pieceTotal = 1) {
  gStored.push_back({macToDisplayStr(mac).c_str(), name ? name : "", message ? message : "",
                     encrypted, (int)type, reqId, piece, pieceTotal});
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

// INSERT_BOND_PEER

// INSERT_POLICY

// INSERT_ORIGIN_KNOWN

// INSERT_TEXT_HANDLER

// INSERT_HANDLER_STUBS

// INSERT_TABLE

// INSERT_LOOKUP

// The production cleanup walks the reassembly table; this double only counts.
static void v4_dispatch_post_cleanup(const esp_now_recv_info*, const EspNowV4Header*) { ++gCleanups; }

// INSERT_DISPATCH

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
// themselves, plaintext-compatible beacons and the topology/user-sync opcodes
// whose handlers enforce their own gates. A subset check: a row may leave this
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
  uint8_t payload[16] = {};
  bool claimed = v4_dispatch_table_try(&ri, &h, payload, sizeof payload, paired, authed, enc, "peer");
  return claimed && gHandlerCalls == 1;
}

// ---- TEXT/BOOT receive path: dispatcher -> real v4h_text -> real drain ------
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
  gEvents.clear();
  gWarns = gPrints = gPushKicks = gCleanups = gHandlerCalls = 0;
  gLastHandler.clear();
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
  std::memcpy(h.origin, origin, 6);
  wifi_pkt_rx_ctrl_t ctrl{-50};
  esp_now_recv_info info{radioSrc, nullptr, &ctrl};
  const bool paired = std::memcmp(radioSrc, gPeerA, 6) == 0;
  const std::string name = paired ? std::string("alpha") : macText(radioSrc);
  return v4_dispatch_table_try(&info, &h, reinterpret_cast<const uint8_t*>(text),
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

  // BOOT shares the TEXT policy.
  resetText(true);
  check("BOOT plaintext keyed from a paired MAC: claimed by the BOOT row",
        deliver(ESPNOW_V4_TYPE_BOOT, gPeerA, gPeerA, "{\"type\":\"boot\"}", false, false));
  check("BOOT plaintext keyed from a paired MAC: dropped (nothing queued, one WARN)",
        queued() == 0 && gWarns == 1);

  // A session-encrypted BOOT is a system notice, never a chat event.
  resetText(true);
  check("BOOT session-encrypted keyed: claimed",
        deliver(ESPNOW_V4_TYPE_BOOT, gPeerA, gPeerA, "{\"type\":\"boot\"}", true, true));
  check("BOOT session-encrypted keyed: queued as MSG_SYSTEM_EVENT",
        queued() == 1 && queuedAt(0).msgType == MSG_SYSTEM_EVENT);
  drainTextQueueForTest();
  check("BOOT session-encrypted keyed: stored as a system event, no SYSEVT_TEXT_RX",
        gStored.size() == 1 && gStored[0].type == MSG_SYSTEM_EVENT && gEvents.empty());

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

  // Table policy for the two chat rows. The flag's own name is not used so the
  // harness also compiles against a table that predates it.
  const V4OpcodeEntry* textRow = v4_dispatch_lookup(ESPNOW_V4_TYPE_TEXT);
  const V4OpcodeEntry* bootRow = v4_dispatch_lookup(ESPNOW_V4_TYPE_BOOT);
  check("TEXT row carries a receive policy (non-zero flags)", textRow && textRow->flags != 0);
  check("BOOT row carries a receive policy (non-zero flags)", bootRow && bootRow->flags != 0);
  check("TEXT and BOOT rows share one policy", textRow && bootRow && textRow->flags == bootRow->flags);
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
// Locally administered test addresses: gPartner is the configured bond partner,
// gOtherPaired is a second paired mesh peer with a live session.
static uint8_t gPartner[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x02};
static uint8_t gOtherPaired[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x03};

// Dispatches one bond frame from `src`; true when exactly one handler ran.
// `minuteLater` moves the clock past the gate's one-line-a-minute limit first.
static bool bondRun(uint8_t type, uint8_t* src, bool paired, bool enc, bool minuteLater = true) {
  if (minuteLater) gNowMs += 61000;
  gLastHandler.clear();
  gHandlerCalls = gCleanups = gWarns = gParseCalls = 0;
  esp_now_recv_info ri{src, nullptr, nullptr};
  EspNowV4Header h{};
  h.type = type;
  h.fragCount = 1;
  uint8_t payload[16] = {};
  const bool claimed = v4_dispatch_table_try(&ri, &h, payload, sizeof payload, paired, enc, enc, "peer");
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
#else
  // Bonded mode compiled out: no bond row exists, so the frames fall to the
  // unknown-type drop whatever the settings say.
  for (uint8_t op : kBondOpcodes) {
    check(std::string(opcodeName(op)) + " has no table row (bonded mode compiled out)",
          v4_dispatch_lookup(op) == nullptr && !bondRun(op, gPartner, true, true) &&
              !bondRun(op, gOtherPaired, true, true));
  }
#endif
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

  // SUBSCRIBE_UPDATE persists the peer's event mask to flash: only a paired,
  // session-encrypted frame may reach the handler.
  expect("SUBSCRIBE_UPDATE paired plaintext keyed",
         run(ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE, true, false, false, true), false);
  expect("SUBSCRIBE_UPDATE paired BROADCAST_AUTH-only keyed",
         run(ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE, true, true, false, true), false);
  expect("SUBSCRIBE_UPDATE paired session-encrypted keyed",
         run(ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE, true, true, true, true), true);
  expect("SUBSCRIBE_UPDATE unpaired session-encrypted keyed",
         run(ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE, false, true, true, true), false);
  expect("SUBSCRIBE_UPDATE paired plaintext unkeyed",
         run(ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE, true, false, false, false), false);

  // SENSOR_STATUS: authenticated once this device has a passphrase, plaintext
  // still accepted on a keyless device.
  expect("SENSOR_STATUS plaintext keyed",
         run(ESPNOW_V4_TYPE_SENSOR_STATUS, false, false, false, true), false);
  check("SENSOR_STATUS plaintext keyed drop is logged (one WARN) and cleaned up",
        gWarns == 1 && gCleanups == 1);
  expect("SENSOR_STATUS BROADCAST_AUTH keyed",
         run(ESPNOW_V4_TYPE_SENSOR_STATUS, false, true, false, true), true);
  expect("SENSOR_STATUS session-encrypted keyed",
         run(ESPNOW_V4_TYPE_SENSOR_STATUS, true, true, true, true), true);
  expect("SENSOR_STATUS plaintext unkeyed (bootstrap compatibility)",
         run(ESPNOW_V4_TYPE_SENSOR_STATUS, false, false, false, false), true);

  // The retired plaintext SENSOR_BROADCAST (150) has no row: the frame falls
  // to the unknown-type drop. The literal is used because the enum symbol is gone.
  check("opcode 150 (retired SENSOR_BROADCAST) has no table row",
        v4_dispatch_lookup(150) == nullptr);

  // Table policy as written.
  const uint8_t kPairedEnc = V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC;
  const V4OpcodeEntry* e = v4_dispatch_lookup(ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE);
  check("SUBSCRIBE_UPDATE row requires REQ_PAIRED|REQ_SESSION_ENC",
        e && (e->flags & kPairedEnc) == kPairedEnc);
  e = v4_dispatch_lookup(ESPNOW_V4_TYPE_SENSOR_STATUS);
  check("SENSOR_STATUS row has non-zero flags", e && e->flags != 0);

  // Verifier's table-driven invariant: no state-mutating opcode has empty flags.
  bool zeroFlagOk = true;
  for (size_t i = 0; i < kV4HandlerTableSize; i++) {
    const V4OpcodeEntry& row = kV4HandlerTable[i];
    if (row.flags != 0 || zeroFlagAllowed(row.opcode)) continue;
    zeroFlagOk = false;
    std::printf("FAIL zero-flag row outside the allow-list: opcode %u (%s) accepts plaintext from any MAC\n",
                (unsigned)row.opcode, opcodeName(row.opcode));
    ++failures;
  }
  if (zeroFlagOk) std::printf("ok   every zero-flag row is on the allow-list\n");

  failures += runTextCases();
  failures += runBondCases();

  std::printf("ENABLE_BONDED_MODE=%d: %u failure%s\n", ENABLE_BONDED_MODE, failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
