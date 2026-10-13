// Host harness for test_espnow_dispatch_policy.py. The runner inserts the
// production opcode/flag enums and header (System_ESPNow_Wire.h slices),
// V4RxCtx, LogMessageType and TextQueueEntry (System_ESPNow.h), the
// V4_OPC_FLAG_* constants and V4OpcodeEntry, espnowOriginIsKnown and the real
// v4h_text, one recording double per other handler named in kV4HandlerTable,
// the table itself, v4_dispatch_lookup, v4_dispatch_table_try and the
// espnow_task text-queue drain at the INSERT_* tags, and defines
// ENABLE_BONDED_MODE on the command line (compiled once per value).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
#define DEBUGF(flag, ...) debugSink(__VA_ARGS__)
#define BROADCAST_PRINTF(...) do { ++gPrints; debugSink(__VA_ARGS__); } while (0)
#define DEBUG_ESPNOW_ROUTER 1
#define ESPNOW_TEXT_MAX_LEN 1024

#define WARN_ESPNOWF(...) do { ++gWarns; } while (0)
[[maybe_unused]] static struct { bool bondModeEnabled = true; } gSettings;

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
static void formatMacAddressBuf(const uint8_t* mac, char* buf, size_t n) {
  std::snprintf(buf, n, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static String formatMacAddress(const uint8_t* mac) {
  char b[18];
  formatMacAddressBuf(mac, b, sizeof b);
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
  gStored.push_back({formatMacAddress(mac).c_str(), name ? name : "", message ? message : "",
                     encrypted, (int)type, reqId, piece, pieceTotal});
  return true;
}
static unsigned gPushKicks = 0;
static void g2ESPNowAppOnRxText(const uint8_t*) { ++gPushKicks; }
static constexpr uint8_t SYSEVT_TEXT_RX = 7;
struct PostedEvent { uint8_t kind; std::string subject, detail; };
static std::vector<PostedEvent> gEvents;
static void systemEventPost(uint8_t kind, const char* subject = nullptr, const char* detail = nullptr,
                            uint8_t = 0, const char* = nullptr) {
  gEvents.push_back({kind, subject ? subject : "", detail ? detail : ""});
}

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

static std::string macText(const uint8_t* mac) { return formatMacAddress(mac).c_str(); }

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

  std::printf("ENABLE_BONDED_MODE=%d: %u failure%s\n", ENABLE_BONDED_MODE, failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
