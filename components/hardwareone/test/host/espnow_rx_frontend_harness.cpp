// Host harness for test_espnow_rx_frontend.py. The runner inserts, at the
// INSERT_* tags, the production wire types and constants, V4RxCtx, the peer
// slot types, v4_crc16_ccitt, the duplicate rings, the peer slot owners
// (getMeshPeerHealth, getMeshPeerMeta, noteMeshPeerRxActivity, processMetadata,
// isPairedDevice, removeEspNowDevice and the reclaim/release helpers), the
// opcode table with its lookup and dispatcher, the relay allow-lists,
// v4_relay_maybe_forward, v4h_relay_data, v4h_heartbeat and the whole of
// v4_try_handle_incoming. Every other table handler is a recording double.
//
// Doubles: sessions (a keyed toy seal over header bytes 0..29 and the payload,
// with the 64-frame replay window), the group-key tag (the same toy MAC), the
// radio (ACK and fan-out counters), the route table, the reassembler (refuses)
// and the registry save. All addresses are locally administered test values.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>
#include <type_traits>
#include <vector>
#include "System_MacText.h"  // production MAC text helpers (copied beside this unit)
#include "System_DeviceName.h"  // the device-name rule (copied beside this unit)
#include "System_ESPNow_AnswerCore.h"  // the answer header {reqId, status} (copied beside this unit)

#ifndef ENABLE_BONDED_MODE
#error "ENABLE_BONDED_MODE must be defined by the runner"
#endif

class String {
  std::string text_;
 public:
  String() = default;
  String(const char* s) : text_(s ? s : "") {}
  size_t length() const { return text_.size(); }
  const char* c_str() const { return text_.c_str(); }
};

#define EXT_RAM_BSS_ATTR
template <class... A> static void logSink(A&&...) {}
static unsigned gWarns = 0;
static std::string gLastWarn;
template <class... A> static void warnSink(const char* fmt, A&&...) { ++gWarns; gLastWarn = fmt; }
#define DEBUG_ESPNOW_ROUTERF(...) logSink(__VA_ARGS__)
#define DEBUG_ESPNOW_MESHF(...) logSink(__VA_ARGS__)
#define DEBUG_ESPNOW_METADATAF(...) logSink(__VA_ARGS__)
#define DEBUG_ESPNOWF(...) logSink(__VA_ARGS__)
#define BROADCAST_PRINTF(...) logSink(__VA_ARGS__)
#define WARN_ESPNOWF(...) warnSink(__VA_ARGS__)
#define ERROR_ESPNOWF(...) warnSink(__VA_ARGS__)
// processMetadata hands room and tags to the membership rules, which report
// through these two (run by test_espnow_group_membership.py).
#define INFO_ESPNOWF(...) logSink(__VA_ARGS__)
template <class... A> static void logSystemEvent(A&&...) {}
static const char* MAC_STR(const uint8_t* m) {
  static char buf[18];
  macToDisplay(m, buf, sizeof buf);
  return buf;
}

static uint32_t gNowMs = 100000;
static uint32_t millis() { return gNowMs; }
// System_Debug.cpp's limiter: true at most once per window.
[[maybe_unused]] static bool logCooldownOk(uint32_t& lastMs, uint32_t windowMs) {
  if (lastMs != 0 && (uint32_t)(gNowMs - lastMs) < windowMs) return false;
  lastMs = gNowMs ? gNowMs : 1;
  return true;
}

struct wifi_pkt_rx_ctrl_t { int8_t rssi; };
struct esp_now_recv_info { uint8_t* src_addr; uint8_t* des_addr; wifi_pkt_rx_ctrl_t* rx_ctrl; };
using esp_now_recv_info_t = esp_now_recv_info;

// INSERT_WIRE

// INSERT_PEER_TYPES

// ---- Owner state: only the members the extracted code touches ---------------
struct EspNowDevice {
  uint8_t mac[6];
  EspNowInlineText<32> name;
  EspNowInlineText<48> friendlyName;
  EspNowInlineText<32> room;
  EspNowInlineText<32> zone;
  EspNowInlineText<64> tags;
  EspNowInlineText<32> acceptedRoom;  // room and tag membership, see test_espnow_group_membership.py
  EspNowInlineText<64> acceptedTags;
  bool stationary;
};
struct EspNowState {
  bool initialized = true;
  bool encryptionEnabled = false;
  EspNowDevice devices[16] = {};
  int deviceCount = 0;
  struct { uint32_t v4FragRx = 0, v4FragRxCompleted = 0; } routerMetrics;
  uint32_t heartbeatsReceived = 0;
  uint32_t bondUnpairedRejectCount = 0;
  uint8_t bondUnpairedRejectType = 0;
  uint8_t bondUnpairedRejectMac[6] = {};
};
static EspNowState gState;
static EspNowState* gEspNow = &gState;

enum MeshRole { MESH_ROLE_WORKER = 0, MESH_ROLE_MASTER = 1, MESH_ROLE_BACKUP_MASTER = 2 };
static struct {
  bool bondModeEnabled = true;
  bool meshRelay = true;
  bool meshBackupEnabled = false;
  uint8_t meshRole = MESH_ROLE_WORKER;
  String meshMasterMac;
  String espnowDeviceName = String("self");
} gSettings;
// The bond-partner gate (REQ_BOND_PEER) asks this; these cases configure no
// partner, so every bond opcode is refused, as in the dispatch-policy test.
static bool isBondPeerMac(const uint8_t*) { return false; }

static constexpr int kSlots = 8;  // the default meshPeerMax
static MeshPeerHealth gHealthSlots[kSlots];
static MeshPeerMeta gMetaSlots[kSlots];
static MeshPeerHealth* gMeshPeers = gHealthSlots;
static MeshPeerMeta* gMeshPeerMeta = gMetaSlots;
static int gMeshPeerSlots = kSlots;
bool gEspNowInRelayDelivery = false;
MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing);
MeshPeerMeta* getMeshPeerMeta(const uint8_t mac[6], bool createIfMissing = false);

static unsigned gRegistrySaves = 0;
static bool saveEspNowDevices() { ++gRegistrySaves; return true; }
// The membership lock around a registry record (removeEspNowDevice and the
// announcement functions take it): one task here, so taking it does nothing.
// test_espnow_group_membership.py checks how it is used.
struct GroupLockGuard {
  GroupLockGuard() {}
};
static bool meshEnabled() { return true; }

// ---- Identity of this device and its mesh -----------------------------------
static uint8_t gSelf[6] = {0x02, 0xA0, 0x00, 0x00, 0x00, 0x01};
static bool isSelfMac(const uint8_t* mac) { return std::memcmp(mac, gSelf, 6) == 0; }
static void espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, gSelf, 6); }
static constexpr uint16_t kMeshFp = 0x5A17;
struct MeshDerivedKeys { bool valid; uint16_t fingerprint; uint8_t groupKey[32]; };
static MeshDerivedKeys gGroup = {true, kMeshFp, {0x47}};
static const MeshDerivedKeys* meshKeysFindByFingerprint(uint16_t fp) { return fp == kMeshFp ? &gGroup : nullptr; }
static const void* meshByFingerprint(uint16_t fp) { return fp == kMeshFp ? &gGroup : nullptr; }
static MeshRelayMetrics gRelayMetrics;
static MeshRelayMetrics& meshRelayMetrics() { return gRelayMetrics; }

// ---- Toy MAC standing in for HMAC-SHA256 and for the AEAD tag ---------------
static void toyMac(uint8_t out[32], const uint8_t* key, size_t keyLen, const uint8_t* a, size_t aLen,
                   const uint8_t* b, size_t bLen) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    h ^= n; h *= 1099511628211ull;
  };
  mix(key, keyLen); mix(a, aLen); mix(b, bLen);
  for (int i = 0; i < 32; ++i) { h ^= (uint64_t)i; h *= 1099511628211ull; out[i] = (uint8_t)(h >> 32); }
}
static bool espnowCryptoHmacSha256(uint8_t out[32], const uint8_t* key, size_t keyLen, const uint8_t* in1,
                                   size_t len1, const uint8_t* in2, size_t len2, const uint8_t*, size_t) {
  toyMac(out, key, keyLen, in1, len1, in2, len2);
  return true;
}
static int sodium_memcmp(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n); }
static void sodium_memzero(void* p, size_t n) { std::memset(p, 0, n); }

// ---- Session double ---------------------------------------------------------
enum SessionStateLifecycle : uint8_t {
  SESSION_FREE = 0, SESSION_ESTABLISHING = 1, SESSION_ACTIVE = 2, SESSION_REKEYING = 3, SESSION_CLOSED = 4,
};
struct SessionState {
  uint8_t peerMac[6];
  uint16_t sessionId;
  uint8_t state;
  uint8_t key[32];
  uint32_t highWater;
  uint64_t bitmap;
  uint32_t txSeq;
};
static SessionState gSessions[4];
static int gSessionCount = 0;
static unsigned gUnwrapCalls = 0, gReplayRejects = 0;
static SessionState* sessionFindBySessionId(uint16_t sessionId, const uint8_t peerMac[6]) {
  for (int i = 0; i < gSessionCount; ++i) {
    if (gSessions[i].sessionId == sessionId && std::memcmp(gSessions[i].peerMac, peerMac, 6) == 0) return &gSessions[i];
  }
  return nullptr;
}
static SessionState* sessionFindByPeer(const uint8_t* peerMac, uint8_t) {
  for (int i = 0; i < gSessionCount; ++i) {
    if (std::memcmp(gSessions[i].peerMac, peerMac, 6) == 0) return &gSessions[i];
  }
  return nullptr;
}
// Same acceptance rule as replayCheckAndInsert in System_ESPNow_Sessions.cpp
// (which test_espnow_session_replay runs for real).
static bool toyReplayAccept(SessionState* s, uint32_t seq) {
  if (seq == 0) return false;
  if (s->highWater == 0) { s->highWater = seq; s->bitmap = 1; return true; }
  if (seq > s->highWater) {
    const uint32_t shift = seq - s->highWater;
    s->bitmap = shift >= 64 ? 1ull : ((s->bitmap << shift) | 1ull);
    s->highWater = seq;
    return true;
  }
  const uint32_t diff = s->highWater - seq;
  if (diff >= 64 || (s->bitmap & (1ull << diff))) return false;
  s->bitmap |= 1ull << diff;
  return true;
}
static bool sessionUnwrapFrame(SessionState* s, const EspNowV4Header* header, const uint8_t* cipherWithTag,
                               uint16_t payloadLen, uint8_t* outPlain, uint16_t* outPlainLen) {
  ++gUnwrapCalls;
  if (payloadLen < ESPNOW_V4_AEAD_TAG_LEN) return false;
  const uint16_t plainLen = (uint16_t)(payloadLen - ESPNOW_V4_AEAD_TAG_LEN);
  uint8_t tag[32];
  toyMac(tag, s->key, sizeof s->key, reinterpret_cast<const uint8_t*>(header), 30, cipherWithTag, plainLen);
  if (std::memcmp(tag, cipherWithTag + plainLen, ESPNOW_V4_AEAD_TAG_LEN) != 0) return false;
  if (!toyReplayAccept(s, header->frameSeq)) { ++gReplayRejects; return false; }
  std::memcpy(outPlain, cipherWithTag, plainLen);
  *outPlainLen = plainLen;
  return true;
}

// The SESSION_UNKNOWN hint (System_ESPNow_Sessions.cpp, its rules run by
// espnow_peer_restart_tests): recorded, so a case can see which sealed frames
// the receive path asks about.
static unsigned gUnknownSessionAsks = 0;
static uint16_t gUnknownSessionLastId = 0;
[[maybe_unused]] static bool sessionUnknownFrameSeen(const uint8_t*, const EspNowV4Header* h) {
  ++gUnknownSessionAsks;
  gUnknownSessionLastId = h->sessionId;
  return true;
}

// ---- Radio, ACK bookkeeping, route table and reassembler doubles ------------
static unsigned gAcksSent = 0, gFragAcksSent = 0, gFanouts = 0, gCleanups = 0, gRelayForwards = 0;
static bool v4_send_ack(const uint8_t*, uint32_t) { ++gAcksSent; return true; }
static bool v4_send_frag_ack(const uint8_t*, uint32_t, uint8_t, uint8_t) { ++gFragAcksSent; return true; }
static size_t gLastFanoutLen = 0;
static void v4_relay_fanout(const uint8_t*, size_t len, const uint8_t*, const uint8_t*, uint16_t) {
  ++gFanouts;
  gLastFanoutLen = len;
}
static bool v4_flood_rate_ok() { return true; }
// No route unless a case names the next hop.
static const uint8_t* gRouteNext = nullptr;
static bool meshRouterLookup(const uint8_t*, uint8_t* nextHop, uint8_t* hops) {
  if (!gRouteNext) return false;
  std::memcpy(nextHop, gRouteNext, 6);
  *hops = 1;
  return true;
}
static bool v4_relay_wrap_and_send(const uint8_t*, const uint8_t*, const uint8_t*, size_t, uint8_t, uint8_t,
                                   uint32_t) { ++gRelayForwards; return true; }
static void broadcast_tracker_record_ack(uint32_t, const uint8_t*) {}
static void sendStatusMarkDelivered(uint32_t, const uint8_t*) {}
static void espnowAppPingNoteAck(const uint8_t*, uint32_t) {}
struct EspNowTxGuard { explicit EspNowTxGuard(const char*) {} };
#define V4_FRAG_ACK_WAIT_MAX 8
struct V4FragAckWait { bool active; uint32_t msgId; uint8_t fragIndex; bool acked; };
static V4FragAckWait gV4FragAckWait[V4_FRAG_ACK_WAIT_MAX];
static void v4_dispatch_post_cleanup(const esp_now_recv_info*, const EspNowV4Header*) { ++gCleanups; }

// ---- Doubles for the parts of v4h_heartbeat that are not under test ---------
struct PeerIdentity { uint8_t meshId; };
static const PeerIdentity* peerIdentityFindByMac(const uint8_t*) { return nullptr; }
struct SessionPrewarm { uint8_t fails; uint32_t cooldownUntilMs; };
static SessionPrewarm gPrewarm;
static constexpr uint8_t kSessionPrewarmMaxFails = 3;
static constexpr uint32_t kSessionPrewarmCooldownMs = 60000;
static SessionPrewarm* sessionPrewarmSlot(const uint8_t*) { return &gPrewarm; }
static void sessionPrewarmReset(const uint8_t*) {}
static bool sessionPrewarmInCooldown(const SessionPrewarm&, uint32_t) { return false; }
static bool sessionPrewarmNoteAttempt(SessionPrewarm*, uint32_t) { return false; }
static bool espnowSessionOpenInitiate(const uint8_t*, const char*) { return true; }
// Bond partner change hold and the handshake back-off (both tested by their
// own suites): no change under way, and hearing a peer has no effect here.
[[maybe_unused]] static bool bondPartnerChanging() { return false; }
// The bond agreement's hook in the drop path (test_espnow_bond_agreement.py and
// test_espnow_dispatch_policy.py cover it).
[[maybe_unused]] static void bondAgreementNoteIgnoredFrame(const uint8_t*, bool) {}
[[maybe_unused]] static void espnowHandshakeHeard(const uint8_t*) {}
static bool gBackupPromoted = false;
static uint32_t gLastMasterHeartbeat = 0;
static bool parseMacAddress(const String&, uint8_t*) { return false; }
static void setMeshRole(MeshRole, const char*) {}
static constexpr uint8_t SYSEVT_MESH_DEMOTED = 1;
static void systemEventPost(uint8_t, const char*, const char*) {}

// INSERT_CRC

// INSERT_DEDUP

// INSERT_SLOTS

// ---- Recording double for every table handler but the two real ones ---------
struct HandlerCall {
  std::string name;
  uint8_t src[6];
  uint32_t msgId;
  uint8_t fragIndex;
  bool authed, enc;
  bool paired;
  uint16_t payloadLen;
};
static std::vector<HandlerCall> gCalls;
static void noteHandler(const char* name, const V4RxCtx& ctx) {
  HandlerCall c{name, {}, ctx.h->msgId, ctx.h->fragIndex, ctx.isAuthenticated, ctx.isSessionEncrypted,
                ctx.isPaired, ctx.payloadLen};
  std::memcpy(c.src, ctx.recv_info->src_addr, 6);
  gCalls.push_back(c);
}

// The receive-drop counters of System_ESPNow.cpp (espnowstats).
static uint32_t gEspNowRxLenDrops = 0;

// INSERT_POLICY

static bool v4_try_handle_incoming(const esp_now_recv_info* recv_info, const uint8_t* data, int len);

// INSERT_RELAY

// INSERT_HEARTBEAT

// INSERT_HANDLER_STUBS

// INSERT_TABLE

// INSERT_LOOKUP

// INSERT_DISPATCH

// INSERT_OUTCOME

// No test here feeds a reassembled message; the real reassembler is run by
// test_espnow_reassembly.
static V4ReasmOutcome v4_reasm_accept(const esp_now_recv_info*, const EspNowV4Header*, const uint8_t*&,
                                      uint16_t&) { return V4ReasmOutcome::Dropped; }

// The mesh gate's counter and its WARN (production), with the registry lookup
// it asks whether the sender is paired.
static bool pairModeIsPaired(const uint8_t* mac) {
  for (int i = 0; i < gState.deviceCount; i++) {
    if (std::memcmp(gState.devices[i].mac, mac, 6) == 0) return true;
  }
  return false;
}
// INSERT_MESH_ID_GATE
// INSERT_INCOMING

// ============================================================================
// Test fixtures
// ============================================================================
static uint8_t gPeerB[6]    = {0x02, 0xA0, 0x00, 0x00, 0x00, 0x0B};  // paired, has a session
static uint8_t gPeerC[6]    = {0x02, 0xA0, 0x00, 0x00, 0x00, 0x0C};  // paired, has its own session
static uint8_t gRelayR[6]   = {0x02, 0xA0, 0x00, 0x00, 0x00, 0x1A};  // paired neighbour that relays
static uint8_t gRelayR2[6]  = {0x02, 0xA0, 0x00, 0x00, 0x00, 0x1B};  // second relaying neighbour
static uint8_t gFar[6]      = {0x02, 0xA0, 0x00, 0x00, 0x00, 0x2F};  // same mesh, out of range, not paired
static uint8_t gAttacker[6] = {0x02, 0xEE, 0x00, 0x00, 0x00, 0x66};  // holds no key

static unsigned gFailures = 0;
static void check(const char* what, bool ok) {
  if (!ok) ++gFailures;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static void addDevice(const uint8_t* mac, const char* name) {
  EspNowDevice& d = gState.devices[gState.deviceCount++];
  d = EspNowDevice{};
  std::memcpy(d.mac, mac, 6);
  d.name = name;
}
static SessionState* addSession(const uint8_t* mac, uint16_t id, uint8_t keyByte) {
  SessionState& s = gSessions[gSessionCount++];
  s = SessionState{};
  std::memcpy(s.peerMac, mac, 6);
  s.sessionId = id;
  s.state = SESSION_ACTIVE;
  std::memset(s.key, keyByte, sizeof s.key);
  return &s;
}

// A keyed device with B, C and both relays paired; B and C hold sessions.
static SessionState* gSessB = nullptr;
static SessionState* gSessC = nullptr;
static void reset(bool keyed) {
  gState = EspNowState{};
  gState.encryptionEnabled = keyed;
  gSettings.meshRelay = true;
  for (auto& s : gHealthSlots) std::memset(&s, 0, sizeof s);
  for (auto& m : gMetaSlots) m.clear();
  gMeshPeerSlots = kSlots;
  std::memset(gV4Dedup, 0, sizeof gV4Dedup);
  gV4DedupIdx = 0;
  gRelayMetrics = MeshRelayMetrics{};
  gSessionCount = 0;
  gCalls.clear();
  gWarns = gAcksSent = gFragAcksSent = gFanouts = gCleanups = gRelayForwards = 0;
  gUnwrapCalls = gReplayRejects = gRegistrySaves = 0;
  gUnknownSessionAsks = 0;
  gUnknownSessionLastId = 0;
  gEspNowRxUnknownMeshIdDrops = 0;
  gLastWarn.clear();
  gEspNowInRelayDelivery = false;
  gRouteNext = nullptr;
  gNowMs += 60000;  // every ring entry of an earlier case is past its TTL
  addDevice(gPeerB, "bravo");
  addDevice(gPeerC, "charlie");
  addDevice(gRelayR, "relay");
  addDevice(gRelayR2, "relay2");
  gSessB = addSession(gPeerB, 7, 0xB1);
  gSessC = addSession(gPeerC, 9, 0xC1);
}

// ---- Frame builders ---------------------------------------------------------
using Frame = std::vector<uint8_t>;
static EspNowV4Header baseHeader(uint8_t type, const uint8_t* origin, uint32_t msgId) {
  EspNowV4Header h{};
  h.magic = (uint16_t)ESPNOW_V4_MAGIC;
  h.ver = ESPNOW_V4_VERSION;
  h.type = type;
  h.headerLen = (uint8_t)sizeof(EspNowV4Header);
  h.msgId = msgId;
  std::memcpy(h.origin, origin, 6);
  h.ttl = 1;
  h.fragIndex = 0;
  h.fragCount = 1;
  return h;
}
static Frame assemble(EspNowV4Header h, const Frame& payload, bool crc) {
  h.crc16 = (crc && !payload.empty()) ? v4_crc16_ccitt(payload.data(), payload.size()) : 0;
  Frame f(sizeof h + payload.size());
  std::memcpy(f.data(), &h, sizeof h);
  if (!payload.empty()) std::memcpy(f.data() + sizeof h, payload.data(), payload.size());
  return f;
}
// What anyone in range can build: no key, optional payload. It carries the
// mesh id, which is on the air in every frame of the mesh (a frame with id 0
// or another mesh's id stops at the mesh gate: testMeshIdGate).
static Frame plainFrame(uint8_t type, const uint8_t* origin, uint32_t msgId, const Frame& payload = {},
                        uint8_t fragIndex = 0, uint8_t fragCount = 1, uint16_t flags = 0) {
  EspNowV4Header h = baseHeader(type, origin, msgId);
  h.meshFingerprint = kMeshFp;
  h.flags = flags;
  h.fragIndex = fragIndex;
  h.fragCount = fragCount;
  return assemble(h, payload, true);
}
// BROADCAST_AUTH frame under the mesh group key.
static Frame groupFrame(uint8_t type, const uint8_t* origin, uint32_t msgId, const Frame& payload, uint8_t ttl = 1) {
  EspNowV4Header h = baseHeader(type, origin, msgId);
  h.flags = ESPNOW_V4_FLAG_BROADCAST_AUTH;
  h.meshFingerprint = kMeshFp;
  h.ttl = ttl;
  uint8_t tag[32];
  toyMac(tag, gGroup.groupKey, 32, reinterpret_cast<const uint8_t*>(&h), 30, payload.data(), payload.size());
  Frame body = payload;
  body.insert(body.end(), tag, tag + ESPNOW_V4_BROADCAST_AUTH_TAG_LEN);
  return assemble(h, body, true);
}
// SESSION_FRAME sealed under `s` with the next frameSeq. `origin` is the header
// field, which a session holder is free to set.
static Frame sealedFrame(SessionState* s, uint8_t type, const uint8_t* origin, uint32_t msgId, const Frame& plain,
                         uint8_t fragIndex = 0, uint8_t fragCount = 1, uint16_t flags = ESPNOW_V4_FLAG_ACK_REQ) {
  EspNowV4Header h = baseHeader(type, origin, msgId);
  h.flags = (uint16_t)(flags | ESPNOW_V4_FLAG_SESSION_FRAME);
  h.meshFingerprint = kMeshFp;
  h.fragIndex = fragIndex;
  h.fragCount = fragCount;
  h.sessionId = s->sessionId;
  h.frameSeq = ++s->txSeq;
  uint8_t tag[32];
  toyMac(tag, s->key, sizeof s->key, reinterpret_cast<const uint8_t*>(&h), 30, plain.data(), plain.size());
  Frame body = plain;
  body.insert(body.end(), tag, tag + ESPNOW_V4_AEAD_TAG_LEN);
  return assemble(h, body, false);
}
// RELAY_DATA envelope as the last hop `relay` emits it toward us.
static Frame envelope(const uint8_t* relay, const Frame& inner, uint32_t msgId) {
  V4PayloadRelayData env{};
  std::memcpy(env.finalDst, gSelf, 6);
  env.hopsUsed = 1;
  Frame body(sizeof env + inner.size());
  std::memcpy(body.data(), &env, sizeof env);
  std::memcpy(body.data() + sizeof env, inner.data(), inner.size());
  return groupFrame(ESPNOW_V4_TYPE_RELAY_DATA, relay, msgId, body, 2);
}
static Frame heartbeatPayload() {
  V4PayloadHeartbeat hb{};
  hb.rssi = -60;
  std::snprintf(hb.deviceName, sizeof hb.deviceName, "peer");
  Frame f(sizeof hb);
  std::memcpy(f.data(), &hb, sizeof hb);
  return f;
}
static Frame text(const char* s) { return Frame(s, s + std::strlen(s)); }
// A KEY_EX_HELLO payload of its exact length (the receive table admits no other).
static Frame helloPayload() { return Frame(ESPNOW_V4_MINLEN_KEY_EX_HELLO, 0x48); }

static bool deliver(uint8_t* radioSrc, const Frame& frame) {
  wifi_pkt_rx_ctrl_t ctrl{-55};
  esp_now_recv_info info{radioSrc, gSelf, &ctrl};
  return v4_try_handle_incoming(&info, frame.data(), (int)frame.size());
}
static unsigned calls(const char* name) {
  unsigned n = 0;
  for (const auto& c : gCalls) n += c.name == name;
  return n;
}
static MeshPeerHealth* health(const uint8_t* mac) { return getMeshPeerHealth(mac, false); }
static MeshPeerMeta* meta(const uint8_t* mac) { return getMeshPeerMeta(mac, false); }
static int activeMeta() {
  int n = 0;
  for (const auto& m : gMetaSlots) n += m.isActive;
  return n;
}
static int activeHealth() {
  int n = 0;
  for (const auto& s : gHealthSlots) n += s.isActive;
  return n;
}
static void macN(uint8_t out[6], uint8_t n) {
  const uint8_t base[6] = {0x02, 0xC0, 0x00, 0x00, 0x00, n};
  std::memcpy(out, base, 6);
}
static V4PayloadMetadata metadataNamed(const char* name) {
  V4PayloadMetadata m{};
  std::snprintf(m.deviceName, sizeof m.deviceName, "%s", name);
  std::snprintf(m.room, sizeof m.room, "lab");
  return m;
}
// What cmd_espnow_unpair does to the registry and the runtime slots (its radio,
// crypto, event and reply steps are not part of this unit).
static void unpairForTest(const uint8_t* mac) {
  removeEspNowDevice(mac);
  releasePeerSlotsForTest(mac);
}

// ============================================================================
// Cases
// ============================================================================
static void testForgedPlaintextDoesNotShadowSealed() {
  // WIRE-2: no key is needed for this frame (public magic/version, fingerprint
  // 0, empty payload so no CRC). It names B as origin and B's next msgId.
  reset(true);
  check("forged plaintext CMD naming B's origin and msgId is consumed",
        deliver(gAttacker, plainFrame(ESPNOW_V4_TYPE_CMD, gPeerB, 41)));
  check("forged plaintext CMD reaches no handler", gCalls.empty());
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 41, text("status")));
  check("B's genuine sealed CMD with that msgId is acknowledged", gAcksSent == 1);
  check("B's genuine sealed CMD with that msgId reaches v4h_cmd (not dropped as a duplicate)",
        calls("v4h_cmd") == 1 && gRelayMetrics.floodDupSuppressed == 0);

  // The stronger forger also transmits with B's MAC as the radio source.
  reset(true);
  deliver(gPeerB, plainFrame(ESPNOW_V4_TYPE_CMD, gPeerB, 42));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 42, text("status")));
  check("forged plaintext sent with B's radio MAC: B's sealed CMD still reaches v4h_cmd",
        calls("v4h_cmd") == 1 && gAcksSent == 1);

  // A burst of forged ids ahead of B's counter, across opcodes.
  reset(true);
  for (uint32_t id = 100; id < 140; ++id) deliver(gAttacker, plainFrame(ESPNOW_V4_TYPE_TEXT, gPeerB, id));
  for (uint32_t id = 100; id < 104; ++id) {
    deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_METADATA_PUSH, gPeerB, id, Frame(sizeof(V4PayloadMetadata), 0)));
  }
  check("forty forged ids: all four genuine sealed frames with those ids are dispatched",
        calls("v4h_metadata_resp_push") == 4);
}

static void testAuthenticatedFramesDoNotShadowEachOther() {
  // Another paired peer (own session) writes B's MAC into its header origin.
  reset(true);
  deliver(gPeerC, sealedFrame(gSessC, ESPNOW_V4_TYPE_CMD, gPeerB, 60, text("x")));
  check("C's sealed frame naming B as origin is dispatched as C's", calls("v4h_cmd") == 1);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 60, text("status")));
  check("B's sealed frame with the same msgId is still dispatched (keyed on the session peer)",
        calls("v4h_cmd") == 2);

  // A group-key holder tags a frame that names B and B's msgId.
  reset(true);
  deliver(gPeerC, groupFrame(ESPNOW_V4_TYPE_TEXT, gPeerB, 61, text("hi")));
  check("group-tagged TEXT naming B is dispatched", calls("v4h_text") == 1);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 61, text("secret")));
  check("B's sealed TEXT with the same msgId is still dispatched (group and sealed entries are separate)",
        calls("v4h_text") == 2);
}

static void testSealedRetriesStillSuppressed() {
  // Identical bytes: the session replay window refuses the second copy.
  reset(true);
  const Frame once = sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 70, text("status"));
  deliver(gPeerB, once);
  deliver(gPeerB, once);
  check("identical sealed frame twice: dispatched once, second refused by the replay window",
        calls("v4h_cmd") == 1 && gReplayRejects == 1);

  // Re-sealed retry of a single frame (fresh frameSeq, same msgId): the replay
  // window accepts it, the duplicate ring must not.
  reset(true);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 71, text("status")));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 71, text("status")));
  check("re-sealed single-frame retry: acknowledged again, dispatched once",
        calls("v4h_cmd") == 1 && gAcksSent == 2 && gReplayRejects == 0);

  // Multi-frame TEXT is dispatched per fragment and never enters the
  // reassembler, so have[] cannot catch a re-sealed fragment retry.
  reset(true);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 72, text("part one "), 0, 2));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 72, text("part two"), 1, 2));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 72, text("part two"), 1, 2));
  check("re-sealed TEXT fragment retry: both fragments dispatched once each",
        calls("v4h_text") == 2 && gFragAcksSent == 3);
}

// Rule F3: answers carry the request's number in their payload (reqId) and a
// header msgId of their own, so STREAM and CMD_RESP no longer skip the
// duplicate check (they used to borrow the command's msgId and every one of
// them did). A re-sealed retry of one is dispatched once; two lines of the
// same command's output (one reqId, two msgIds) are both dispatched.
static Frame answer(uint32_t reqId, uint8_t status, const char* body) {
  Frame f = text(body);
  Frame out(espnowanswer::kHeaderLen, 0);
  espnowanswer::packHeader(out.data(), reqId, status);
  out.insert(out.end(), f.begin(), f.end());
  return out;
}
static void testAnswersAreDeduplicated() {
  reset(true);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_STREAM, gPeerB, 300, answer(77, 0, "line one")));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_STREAM, gPeerB, 300, answer(77, 0, "line one")));
  check("re-sealed STREAM retry: acknowledged, dispatched once (FAILS WITH THE BYPASS LIST)",
        calls("v4h_stream") == 1 && gReplayRejects == 0);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_STREAM, gPeerB, 301, answer(77, 0, "line two")));
  check("the next line of the same command (same reqId, its own msgId) is dispatched",
        calls("v4h_stream") == 2);

  reset(true);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD_RESP, gPeerB, 310, answer(77, 0, "done")));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD_RESP, gPeerB, 310, answer(77, 0, "done")));
  check("re-sealed CMD_RESP retry: dispatched once (FAILS WITH THE BYPASS LIST)", calls("v4h_cmd_resp") == 1);
  // The answer's msgId is the answerer's own: a CMD of this device's with the
  // same number does not hide the answer, nor the answer the CMD.
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 311, text("status")));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD_RESP, gPeerB, 312, answer(311, 0, "ok")));
  check("a CMD and the CMD_RESP answering it (reqId 311, msgId 312) are both dispatched",
        calls("v4h_cmd") == 1 && calls("v4h_cmd_resp") == 2);
}

static void testFloodLoopSuppression() {
  reset(true);
  const Frame viaR = groupFrame(ESPNOW_V4_TYPE_TEXT, gFar, 80, text("hello mesh"), 3);
  const Frame viaR2 = groupFrame(ESPNOW_V4_TYPE_TEXT, gFar, 80, text("hello mesh"), 2);
  deliver(gRelayR, viaR);
  check("flooded broadcast TEXT: dispatched and forwarded once",
        calls("v4h_text") == 1 && gRelayMetrics.floodForwards == 1 && gFanouts == 1);
  deliver(gRelayR2, viaR2);
  check("the same broadcast from a second neighbour: suppressed, not forwarded again",
        calls("v4h_text") == 1 && gRelayMetrics.floodForwards == 1 && gRelayMetrics.floodDupSuppressed == 1);

  // Forged plaintext cannot push the entry out of the ring.
  for (uint32_t id = 1000; id < 1100; ++id) deliver(gAttacker, plainFrame(ESPNOW_V4_TYPE_CMD, gAttacker, id));
  deliver(gRelayR, viaR);
  check("after 100 forged plaintext frames the duplicate broadcast is still suppressed",
        calls("v4h_text") == 1 && gRelayMetrics.floodForwards == 1 && gRelayMetrics.floodDupSuppressed == 2);

  // The entry still expires: a peer that rebooted reuses low msgIds.
  gNowMs += 5001;
  const unsigned beforeExpiry = calls("v4h_text");
  deliver(gRelayR, viaR);
  check("after the 5 s TTL the same origin and msgId is accepted as a new broadcast",
        calls("v4h_text") == beforeExpiry + 1);
}

static void testRelayedLoopSuppression() {
  // Routed handshake frame: plaintext inner frame, group-tagged envelope.
  reset(true);
  const Frame hello = plainFrame(ESPNOW_V4_TYPE_KEY_EX_HELLO, gFar, 90, helloPayload());
  const Frame env = envelope(gRelayR, hello, 90);
  deliver(gRelayR, env);
  check("relayed KEY_EX_HELLO: delivered once, source rewritten to the origin",
        calls("v4hKeyExHello") == 1 && std::memcmp(gCalls.back().src, gFar, 6) == 0 &&
        gRelayMetrics.routedDelivered == 1);
  deliver(gRelayR, env);
  check("the same envelope again: inner frame suppressed as a duplicate",
        calls("v4hKeyExHello") == 1 && gRelayMetrics.floodDupSuppressed == 1);
  check("relay delivery leaves no health slot for the far origin", health(gFar) == nullptr);

  // A forged direct frame (radio source spoofed as the far origin) does not
  // shadow the frame that arrives through the authenticated envelope.
  reset(true);
  deliver(gFar, plainFrame(ESPNOW_V4_TYPE_KEY_EX_HELLO, gFar, 91));
  const unsigned forgedCalls = calls("v4hKeyExHello");
  deliver(gRelayR, envelope(gRelayR, plainFrame(ESPNOW_V4_TYPE_KEY_EX_HELLO, gFar, 91, helloPayload()), 91));
  check("forged direct KEY_EX_HELLO with the same origin and msgId: the relayed one is still delivered",
        calls("v4hKeyExHello") == forgedCalls + 1);

  // Sealed inner frame from a paired peer that is out of range.
  reset(true);
  const Frame sealed = sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 92, text("status"));
  deliver(gRelayR, envelope(gRelayR, sealed, 92));
  deliver(gRelayR, envelope(gRelayR, sealed, 92));
  check("relayed sealed CMD twice (identical bytes): dispatched once", calls("v4h_cmd") == 1);
  deliver(gRelayR, envelope(gRelayR, sealedFrame(gSessB, ESPNOW_V4_TYPE_CMD, gPeerB, 92, text("status")), 92));
  check("relayed re-sealed retry of that CMD: still dispatched once", calls("v4h_cmd") == 1);
}

static void testFragmentIndexConsistency() {
  reset(false);
  deliver(gPeerB, plainFrame(ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 110, heartbeatPayload(), 1, 1));
  check("single frame with fragIndex 1 of 1 is refused (no handler)", gState.heartbeatsReceived == 0);
  reset(false);
  deliver(gPeerB, plainFrame(ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 111, heartbeatPayload(), 0, 0));
  check("fragCount 0 with fragIndex 0 is still read as a single frame", gState.heartbeatsReceived == 1);
  deliver(gPeerB, plainFrame(ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 112, heartbeatPayload(), 0, 1));
  check("fragIndex 0 of 1 is accepted", gState.heartbeatsReceived == 2);

  reset(true);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 113, text("x"), 5, 3));
  check("sealed frame with fragIndex 5 of 3 is refused before it is opened or acknowledged",
        gUnwrapCalls == 0 && gFragAcksSent == 0 && gAcksSent == 0 && gCalls.empty());
  reset(true);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 113, text("x"), 2, 3));
  check("sealed TEXT fragment 2 of 3 is dispatched", calls("v4h_text") == 1);

  // The verifier's detail: a forged single frame used to pre-insert the key of
  // a later fragment.
  reset(true);
  deliver(gAttacker, plainFrame(ESPNOW_V4_TYPE_TEXT, gPeerB, 114, {}, 1, 1));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 114, text("part two"), 1, 2));
  check("forged single frame carrying fragIndex 1: B's sealed fragment 1 of 2 is still dispatched",
        calls("v4h_text") == 1);
}

static void testHeartbeatPolicy() {
  reset(true);
  deliver(gPeerB, plainFrame(ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 120, heartbeatPayload()));
  check("keyed: plaintext HEARTBEAT from a paired MAC is dropped before v4h_heartbeat",
        gState.heartbeatsReceived == 0 && gWarns == 1);
  check("keyed: the dropped HEARTBEAT records no liveness",
        health(gPeerB) == nullptr || health(gPeerB)->lastMeshHeartbeatMs == 0);
  reset(true);
  deliver(gPeerB, groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 121, heartbeatPayload()));
  check("keyed: group-tagged HEARTBEAT is accepted and records liveness",
        gState.heartbeatsReceived == 1 && health(gPeerB) && health(gPeerB)->lastMeshHeartbeatMs == gNowMs &&
        health(gPeerB)->everHeardDirect && health(gPeerB)->rssi == -60);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 122, heartbeatPayload()));
  check("keyed: sealed HEARTBEAT is accepted",
        gState.heartbeatsReceived == 2 && health(gPeerB) && health(gPeerB)->heartbeatCount == 2);

  reset(false);
  deliver(gPeerB, plainFrame(ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 123, heartbeatPayload()));
  check("keyless: plaintext HEARTBEAT from a paired peer is accepted and records liveness",
        gState.heartbeatsReceived == 1 && gWarns == 0 && health(gPeerB) &&
        health(gPeerB)->lastMeshHeartbeatMs == gNowMs && meta(gPeerB) != nullptr);
}

static void testSlotCreation() {
  // Keyless device: the HEARTBEAT row lets plaintext through, the slot owner
  // must not create anything for a MAC that is not paired.
  reset(false);
  deliver(gAttacker, plainFrame(ESPNOW_V4_TYPE_HEARTBEAT, gAttacker, 130, heartbeatPayload()));
  check("keyless: plaintext HEARTBEAT from an unknown MAC reaches the handler", gState.heartbeatsReceived == 1);
  check("keyless: plaintext HEARTBEAT from an unknown MAC creates no health slot", health(gAttacker) == nullptr);
  check("keyless: plaintext HEARTBEAT from an unknown MAC creates no metadata slot",
        meta(gAttacker) == nullptr && activeMeta() == 0);

  reset(true);
  for (uint8_t n = 1; n <= 12; ++n) {
    uint8_t mac[6];
    macN(mac, n);
    deliver(mac, plainFrame(ESPNOW_V4_TYPE_ACK, mac, 140u + n));
    deliver(mac, plainFrame(ESPNOW_V4_TYPE_HEARTBEAT, mac, 160u + n, heartbeatPayload()));
  }
  check("keyed: plaintext ACK and HEARTBEAT from twelve invented MACs create no health slot", activeHealth() == 0);
  check("keyed: plaintext ACK and HEARTBEAT from twelve invented MACs create no metadata slot", activeMeta() == 0);

  // What must keep working (also on a tree where the burst above filled the tables).
  reset(true);
  deliver(gPeerB, plainFrame(ESPNOW_V4_TYPE_ACK, gPeerB, 180));
  check("plaintext ACK from a paired peer creates its slots and counts the ACK",
        health(gPeerB) && health(gPeerB)->ackCount == 1 && meta(gPeerB));

  deliver(gFar, groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, gFar, 181, heartbeatPayload()));
  check("group-tagged HEARTBEAT from an unpaired same-mesh neighbour creates its slots",
        health(gFar) && meta(gFar) && health(gFar)->heartbeatCount == 1);
  deliver(gFar, plainFrame(ESPNOW_V4_TYPE_ACK, gFar, 182));
  check("a later plaintext ACK from that neighbour refreshes the existing slot",
        health(gFar) && health(gFar)->ackCount == 1);
}

static void testUnpairReleasesMetadata() {
  reset(true);
  deliver(gPeerB, groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 190, heartbeatPayload()));
  const V4PayloadMetadata m = metadataNamed("bravo-desk");
  processMetadata(gPeerB, &m);
  check("paired peer has a health slot and named metadata",
        health(gPeerB) && meta(gPeerB) && std::strcmp(meta(gPeerB)->name, "bravo-desk") == 0);
  const int before = activeMeta();
  unpairForTest(gPeerB);
  check("unpair releases the health slot", health(gPeerB) == nullptr);
  check("unpair releases the metadata slot", meta(gPeerB) == nullptr && activeMeta() == before - 1);
  bool cleared = true;
  for (const auto& slot : gMetaSlots) cleared = cleared && std::strcmp(slot.name, "bravo-desk") != 0;
  check("the released metadata slot no longer holds the peer's name", cleared);
}

static void testMetadataTableFull() {
  // Eight unpaired same-mesh neighbours take all eight slots (default table).
  reset(true);
  for (uint8_t n = 1; n <= 8; ++n) {
    uint8_t mac[6];
    macN(mac, n);
    gNowMs += 10;
    deliver(mac, groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, mac, 200u + n, heartbeatPayload()));
  }
  check("eight unpaired neighbours hold all eight health and metadata slots",
        activeHealth() == kSlots && activeMeta() == kSlots);
  uint8_t ninth[6];
  macN(ninth, 9);
  deliver(ninth, groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, ninth, 209, heartbeatPayload()));
  check("a ninth unpaired neighbour gets no slot and takes none away",
        health(ninth) == nullptr && meta(ninth) == nullptr && activeMeta() == kSlots);

  // A peer is paired now; its metadata arrives over its session.
  uint8_t fresh[6];
  macN(fresh, 0x50);
  addDevice(fresh, "fresh");
  const V4PayloadMetadata m = metadataNamed("fresh-node");
  gWarns = 0;
  processMetadata(fresh, &m);
  check("a newly paired peer's metadata is stored although the table was full",
        meta(fresh) && std::strcmp(meta(fresh)->name, "fresh-node") == 0 && gWarns == 0);
  uint8_t first[6];
  macN(first, 1);
  check("the slot came from the unpaired neighbour heard longest ago",
        meta(first) == nullptr && activeMeta() == kSlots);

  // The periodic sweep drops health slots that stopped heartbeating; their
  // metadata slots are then free for anyone, also for an unpaired neighbour.
  for (auto& slot : gHealthSlots) slot.isActive = false;
  uint8_t tenth[6];
  macN(tenth, 10);
  deliver(tenth, groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, tenth, 210, heartbeatPayload()));
  check("after the neighbours timed out, a new unpaired neighbour gets a health and a metadata slot",
        health(tenth) && meta(tenth));
  check("the paired peer's metadata survived that reclaim",
        meta(fresh) && std::strcmp(meta(fresh)->name, "fresh-node") == 0);

  // Paired peers are never evicted.
  reset(true);
  for (uint8_t n = 1; n <= 8; ++n) {
    uint8_t mac[6];
    macN(mac, (uint8_t)(0x60 + n));
    addDevice(mac, "paired");
    const V4PayloadMetadata pm = metadataNamed("paired-node");
    processMetadata(mac, &pm);
  }
  uint8_t extra[6];
  macN(extra, 0x70);
  addDevice(extra, "extra");
  const V4PayloadMetadata em = metadataNamed("extra-node");
  gWarns = 0;
  processMetadata(extra, &em);
  bool intact = true;
  for (const auto& slot : gMetaSlots) intact = intact && slot.isActive && std::strcmp(slot.name, "paired-node") == 0;
  check("a table full of paired peers refuses a ninth and keeps all eight", meta(extra) == nullptr && intact);
  check("that refusal is still reported (ALL slots full)",
        gWarns == 1 && gLastWarn.find("slots full") != std::string::npos);
  // The table is the display copy. The ninth peer's registry record, which
  // room and tag commands go by, follows its metadata all the same.
  V4PayloadMetadata moved = metadataNamed("extra-node");
  std::snprintf(moved.room, sizeof(moved.room), "Garage");
  processMetadata(extra, &moved);
  bool recorded = false;
  for (int i = 0; i < gState.deviceCount; ++i) {
    if (std::memcmp(gState.devices[i].mac, extra, 6) == 0) recorded = gState.devices[i].room == "Garage";
  }
  check("a paired peer without a table slot still has its room recorded in the registry", recorded);
}

static void testReplayedHeartbeatFromInventedSource() {
  // A BROADCAST_AUTH tag covers the header (origin included) and the payload,
  // not the radio source. Someone without the key captures a member's tagged
  // heartbeats (unicast in the clear with a tag, a new msgId every 5 s) and
  // retransmits each from a different invented radio MAC. The frames verify,
  // so they reach v4h_heartbeat as authenticated; they must not create slots
  // for MACs nobody proved.
  reset(true);
  Frame captured[8];
  for (uint8_t n = 0; n < 8; ++n) captured[n] = groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, gFar, 300u + n, heartbeatPayload());
  for (uint8_t n = 0; n < 8; ++n) {
    uint8_t invented[6];
    macN(invented, (uint8_t)(0x80 + n));
    deliver(invented, captured[n]);
  }
  check("eight captured tagged HEARTBEATs replayed from eight invented MACs reach the handler",
        gState.heartbeatsReceived == 8);
  check("replayed tagged HEARTBEATs from invented MACs create no health slot", activeHealth() == 0);
  check("replayed tagged HEARTBEATs from invented MACs create no metadata slot", activeMeta() == 0);
  deliver(gPeerB, groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 320, heartbeatPayload()));
  check("after those replays a paired peer's genuine tagged HEARTBEAT still gets its health slot",
        health(gPeerB) && health(gPeerB)->heartbeatCount == 1 && meta(gPeerB));

  // The very same frame: suppressed as a duplicate inside the TTL, accepted as
  // a frame after it, and in neither case a slot for the invented source.
  reset(true);
  const Frame genuine = groupFrame(ESPNOW_V4_TYPE_HEARTBEAT, gFar, 330, heartbeatPayload());
  deliver(gFar, genuine);
  check("the member's own tagged HEARTBEAT (origin equals the radio source) creates its slots",
        health(gFar) && meta(gFar) && activeHealth() == 1);
  uint8_t invented[6];
  macN(invented, 0x90);
  deliver(invented, genuine);
  gNowMs += 5001;
  deliver(invented, genuine);
  check("that frame replayed from an invented MAC, inside and after the 5 s TTL, creates no slot",
        health(invented) == nullptr && meta(invented) == nullptr && activeHealth() == 1 && activeMeta() == 1);

  // A session frame is bound to its radio source by the session lookup, so it
  // proves that MAC whatever its header origin says (a session peer need not be
  // in the device registry).
  reset(true);
  uint8_t sessionOnly[6];
  macN(sessionOnly, 0x91);
  SessionState* sess = addSession(sessionOnly, 11, 0xD1);
  deliver(sessionOnly, sealedFrame(sess, ESPNOW_V4_TYPE_HEARTBEAT, gPeerB, 340, heartbeatPayload()));
  check("sealed HEARTBEAT from a session peer outside the registry creates its slot (header origin ignored)",
        health(sessionOnly) && health(sessionOnly)->heartbeatCount == 1 && health(gPeerB) == nullptr);
}

// Mesh time, the sensor frames, the battery frame and the event frame are
// session unicasts that must work through a relay: the sealed frame, byte-identical inside a RELAY_DATA envelope, is
// delivered to its handler with the origin as the source, and passes the
// paired + session gates there. None of them is flooded.
static void testTimeAndSensorFramesThroughRelay() {
  // The envelope budgets of System_ESPNow_Wire.h (espnow_sensor_wire checks
  // that the header has these numbers): 250 minus the outer header, the
  // envelope and the outer tag; and what is left for a sealed frame's plaintext.
  constexpr size_t ESPNOW_V4_RELAY_MAX_INNER_FRAME = 250 - 32 - 8 - ESPNOW_V4_BROADCAST_AUTH_TAG_LEN;  // 194
  constexpr size_t ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT =
      ESPNOW_V4_RELAY_MAX_INNER_FRAME - 32 - ESPNOW_V4_AEAD_TAG_LEN;                                    // 146
  check("the routed budgets: 194-byte inner frame, 146 bytes of sealed plaintext (16-byte group tag)",
        ESPNOW_V4_RELAY_MAX_INNER_FRAME == 194 && ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT == 146);
  struct Row { uint8_t type; const char* handler; size_t payloadLen; };
  const Row rows[] = {
    { ESPNOW_V4_TYPE_TIME_SYNC,       "v4hTimeSync",         16 },
    { ESPNOW_V4_TYPE_SENSOR_STATUS,   "v4h_sensor_status",   16 },
    { ESPNOW_V4_TYPE_SENSOR_REQ,      "v4h_sensor_req",      20 },
    { ESPNOW_V4_TYPE_SENSOR_ENVELOPE, "v4h_sensor_envelope", 60 },
    // A part of a reading at the routed size: header 12 + 134 bytes of text
    // is exactly what one sealed frame carries inside the envelope.
    { ESPNOW_V4_TYPE_SENSOR_ENVELOPE, "v4h_sensor_envelope", ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT },
    // The battery frame (24 bytes) and the event frame (128 bytes, 18 under
    // the routed budget): one frame each, also through a relay.
    { ESPNOW_V4_TYPE_FORWARD_LINK,    "v4hForwardLink",      96 },
    { ESPNOW_V4_TYPE_POWER_STATUS,    "v4hPowerStatus",      24 },
    { ESPNOW_V4_TYPE_EVENT_PUSH,      "v4hEventPush",        128 },
  };
  uint32_t id = 300;
  for (const Row& row : rows) {
    reset(true);
    const std::string what = std::string(row.handler) + " (" + std::to_string(row.payloadLen) + " B)";
    const Frame inner = sealedFrame(gSessB, row.type, gPeerB, ++id, Frame(row.payloadLen, 0x5A), 0, 1, /*flags=*/0);
    check((what + ": the sealed frame fits a relay envelope").c_str(), inner.size() <= ESPNOW_V4_RELAY_MAX_INNER_FRAME);
    deliver(gRelayR, envelope(gRelayR, inner, id));
    check((what + " through a relay: delivered once to its handler").c_str(),
          calls(row.handler) == 1 && gCalls.size() == 1 && gRelayMetrics.routedDelivered == 1);
    check((what + " through a relay: the handler sees the origin as the sender, paired and session-encrypted").c_str(),
          !gCalls.empty() && std::memcmp(gCalls.back().src, gPeerB, 6) == 0 && gCalls.back().paired &&
          gCalls.back().enc);
    check((what + " through a relay: the payload arrives whole").c_str(),
          !gCalls.empty() && gCalls.back().payloadLen == row.payloadLen);
    check((what + ": nothing is flooded on").c_str(), gRelayMetrics.floodForwards == 0 && gFanouts == 0);
  }
  // One byte more than the routed budget does not fit an envelope: the sender
  // plans its parts by that budget (espnow_sensor_core_tests).
  reset(true);
  const Frame tooBig = sealedFrame(gSessB, ESPNOW_V4_TYPE_SENSOR_ENVELOPE, gPeerB, ++id,
                                   Frame(ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT + 1, 0x5A), 0, 1, 0);
  check("a sealed frame one byte over the routed budget is larger than an envelope carries",
        tooBig.size() > ESPNOW_V4_RELAY_MAX_INNER_FRAME);

  // The same frames as a group-signed broadcast with hops left: not flooded
  // on and not handed to a handler (a signed broadcast does not pass the gates).
  for (uint8_t type : { (uint8_t)ESPNOW_V4_TYPE_TIME_SYNC, (uint8_t)ESPNOW_V4_TYPE_SENSOR_STATUS,
                        (uint8_t)ESPNOW_V4_TYPE_SENSOR_ENVELOPE, (uint8_t)ESPNOW_V4_TYPE_POWER_STATUS,
                        (uint8_t)ESPNOW_V4_TYPE_EVENT_PUSH }) {
    reset(true);
    deliver(gRelayR, groupFrame(type, gFar, ++id, Frame(16, 0x5A), 3));
    check(("opcode " + std::to_string(type) + " as a signed broadcast: not flooded, no handler").c_str(),
          gCalls.empty() && gRelayMetrics.floodForwards == 0 && gFanouts == 0);
  }
}

// A sealed frame for a session this device does not hold (it restarted, bench
// 2026-11-18): it is not opened, acknowledged or handed on, and the receive
// path asks the SESSION_UNKNOWN rule once per such frame, so the sender learns
// it must open a new session. A frame under a live session asks nothing.
// The group (BROADCAST_AUTH) tag is 16 bytes since 1.0: a frame with it is
// accepted, a one-bit change of it is dropped, a frame that carries the old
// 32-byte tag is dropped, and a flooded frame goes on with a 16-byte tag.
static void testGroupTagIs16Bytes() {
  check("ESPNOW_V4_BROADCAST_AUTH_TAG_LEN is 16", ESPNOW_V4_BROADCAST_AUTH_TAG_LEN == 16);
  reset(true);
  const Frame payload = text("hello mesh");
  const Frame good = groupFrame(ESPNOW_V4_TYPE_TEXT, gFar, 700, payload, 1);
  check("a group-signed frame is header + payload + 16 bytes", good.size() == 32 + payload.size() + 16);
  deliver(gRelayR, good);
  check("a frame with the 16-byte group tag is accepted",
        calls("v4h_text") == 1 && gCalls.back().authed);
  reset(true);
  Frame flipped = groupFrame(ESPNOW_V4_TYPE_TEXT, gFar, 701, payload, 1);
  flipped[flipped.size() - 1] ^= 0x01;
  // keep the transport CRC right so only the tag decides
  {
    EspNowV4Header h;
    std::memcpy(&h, flipped.data(), sizeof h);
    h.crc16 = v4_crc16_ccitt(flipped.data() + sizeof h, flipped.size() - sizeof h);
    std::memcpy(flipped.data(), &h, sizeof h);
  }
  deliver(gRelayR, flipped);
  check("one bit changed in the group tag: dropped", gCalls.empty());
  reset(true);
  {
    // The pre-1.0 form: the full 32-byte HMAC appended.
    EspNowV4Header h = baseHeader(ESPNOW_V4_TYPE_TEXT, gFar, 702);
    h.flags = ESPNOW_V4_FLAG_BROADCAST_AUTH;
    h.meshFingerprint = kMeshFp;
    h.ttl = 1;
    uint8_t tag[32];
    toyMac(tag, gGroup.groupKey, 32, reinterpret_cast<const uint8_t*>(&h), 30, payload.data(), payload.size());
    Frame body = payload;
    body.insert(body.end(), tag, tag + 32);
    deliver(gRelayR, assemble(h, body, true));
  }
  check("a frame carrying the old 32-byte group tag: dropped", gCalls.empty());
  reset(true);
  deliver(gRelayR, groupFrame(ESPNOW_V4_TYPE_TEXT, gFar, 703, payload, 3));
  check("a flooded frame is forwarded as header + payload + a 16-byte tag",
        gFanouts == 1 && gLastFanoutLen == 32 + payload.size() + 16);
}

static void testUnknownSessionAsksForHint() {
  reset(true);
  SessionState lost{};
  std::memcpy(lost.peerMac, gPeerB, 6);
  lost.sessionId = 16447;  // the session B still holds; this device lost it
  std::memset(lost.key, 0xB7, sizeof lost.key);
  uint32_t id = 900;
  for (int i = 0; i < 3; ++i) deliver(gPeerB, sealedFrame(&lost, ESPNOW_V4_TYPE_TEXT, gPeerB, ++id, text("burst")));
  check("a sealed frame for an unknown session is not acknowledged or handed on",
        gCalls.empty() && gAcksSent == 0 && gUnwrapCalls == 0);
  check("each one asks the SESSION_UNKNOWN rule (FAILS ON BASE: dropped silently)",
        gUnknownSessionAsks == 3 && gUnknownSessionLastId == 16447);
  // A session that is not ACTIVE (our OPEN awaiting its CONFIRM) is also not
  // opened; the rule decides (it answers nothing for that id).
  reset(true);
  gSessB->state = SESSION_ESTABLISHING;
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, ++id, text("early")));
  check("a frame for a session still ESTABLISHING is not opened and asks the rule",
        gCalls.empty() && gUnwrapCalls == 0 && gUnknownSessionAsks == 1 && gUnknownSessionLastId == 7);
  reset(true);
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, ++id, text("live")));
  check("a frame under the live session is delivered and asks nothing",
        calls("v4h_text") == 1 && gUnknownSessionAsks == 0);
}

// The mesh gate: every 1.0 sender stamps its mesh's key-derived id, never 0
// (System_ESPNow_MeshIdCore.h). Id 0 and ids of no mesh here are dropped
// before ACK and dispatch, and counted; a paired sender (a device of our mesh
// with another passphrase or label) also gets one WARN a minute.
static void testMeshIdGate() {
  reset(true);
  Frame zero = plainFrame(ESPNOW_V4_TYPE_TOPO_REQ, gAttacker, 2001, Frame(8, 0));
  reinterpret_cast<EspNowV4Header*>(zero.data())->meshFingerprint = 0;
  deliver(gAttacker, zero);
  check("a frame with mesh id 0 reaches no handler and is counted (FAILS ON BASE: 0 was 'unscoped')",
        gCalls.empty() && gEspNowRxUnknownMeshIdDrops == 1 && gEspNowRxUnknownMeshIdLast == 0);
  check("an unpaired sender's drop is only counted", gWarns == 0);

  Frame foreign = plainFrame(ESPNOW_V4_TYPE_TEXT, gAttacker, 2002, text("x"));
  reinterpret_cast<EspNowV4Header*>(foreign.data())->meshFingerprint = 0x1234;
  deliver(gAttacker, foreign);
  check("another mesh's id is dropped and counted, with no WARN",
        gCalls.empty() && gEspNowRxUnknownMeshIdDrops == 2 && gEspNowRxUnknownMeshIdLast == 0x1234 && gWarns == 0);

  Frame fromPaired = plainFrame(ESPNOW_V4_TYPE_TEXT, gPeerC, 2003, text("y"));
  reinterpret_cast<EspNowV4Header*>(fromPaired.data())->meshFingerprint = 0x4321;
  deliver(gPeerC, fromPaired);
  check("a paired device with another mesh id is dropped, counted and named in one WARN",
        gCalls.empty() && gEspNowRxUnknownMeshIdDrops == 3 && gWarns == 1 &&
        std::memcmp(gEspNowRxUnknownMeshIdLastMac, gPeerC, 6) == 0);
  reinterpret_cast<EspNowV4Header*>(fromPaired.data())->msgId = 2004;
  deliver(gPeerC, fromPaired);
  check("the WARN is limited to one a minute per address", gEspNowRxUnknownMeshIdDrops == 4 && gWarns == 1);
  gNowMs += 61000;
  reinterpret_cast<EspNowV4Header*>(fromPaired.data())->msgId = 2005;
  deliver(gPeerC, fromPaired);
  check("a minute later it is said again", gWarns == 2);

  uint8_t stranger[6] = {0x02, 0xEE, 0x00, 0x00, 0x00, 0x77};
  Frame hello = plainFrame(ESPNOW_V4_TYPE_KEY_EX_HELLO, stranger, 2006, Frame(8, 0));
  reinterpret_cast<EspNowV4Header*>(hello.data())->meshFingerprint = 0x2222;
  deliver(stranger, hello);
  check("a key exchange with another mesh id (wrong passphrase) gets a WARN", gWarns == 3);
}

// Header flag classes (1.0): a bit in 0xF000 this firmware does not know drops
// the frame before it is opened, acknowledged, remembered or forwarded by
// flooding, and is counted; any other unknown bit is ignored. A relay does
// not look inside a routed envelope; the final receiver checks the inner frame.
static void testMustUnderstandFlags() {
#ifdef HW1_MUST_UNDERSTAND
  check("the must-understand class is 0xF000 and 1.0 knows none of it",
        ESPNOW_V4_FLAGS_MUST_UNDERSTAND == 0xF000 && ESPNOW_V4_FLAGS_KNOWN_MUST_UNDERSTAND == 0 &&
        ESPNOW_V4_FLAGS_UNKNOWN_MUST_UNDERSTAND == 0xF000);
  uint32_t id = 700;
  for (uint16_t bit : {0x1000, 0x2000, 0x4000, 0x8000}) {
    reset(true);
    const uint32_t drops = gEspNowRxMustUnderstandDrops;
    const uint32_t msgId = ++id;
    deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, msgId, text("hi"), 0, 1,
                                (uint16_t)(ESPNOW_V4_FLAG_ACK_REQ | bit)));
    char what[160];
    std::snprintf(what, sizeof what, "flag 0x%04X on a sealed frame: no handler, no ACK, not opened, counted once "
                  "(FAILS WITHOUT THE RULE)", (unsigned)bit);
    check(what, gCalls.empty() && gAcksSent == 0 && gUnwrapCalls == 0 &&
                gEspNowRxMustUnderstandDrops == drops + 1);
    // Not remembered: the same msgId without the bit is a new frame.
    deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, msgId, text("hi")));
    std::snprintf(what, sizeof what, "flag 0x%04X: the frame left no duplicate entry (the same msgId is "
                  "delivered without the bit)", (unsigned)bit);
    check(what, calls("v4h_text") == 1 && gAcksSent == 1);
  }

  // A group-tagged broadcast with the bit is not flooded on.
  reset(true);
  {
    const Frame plain = groupFrame(ESPNOW_V4_TYPE_TEXT, gFar, 750, text("hello mesh"), 3);
    Frame f = plain;
    EspNowV4Header h;
    std::memcpy(&h, f.data(), sizeof h);
    h.flags = (uint16_t)(h.flags | 0x1000);
    std::memcpy(f.data(), &h, sizeof h);
    deliver(gRelayR, f);
    check("a flooded broadcast with flag 0x1000: not dispatched, not forwarded",
          calls("v4h_text") == 0 && gRelayMetrics.floodForwards == 0 && gFanouts == 0);
  }

  // Unknown bits outside the class are ignored (the old ENCRYPTED, COMPRESS,
  // STREAM_END, HANDSHAKE and PRIORITY_HIGH values among them).
  for (uint16_t bit : {0x0002, 0x0004, 0x0010, 0x0020, 0x0040, 0x0400, 0x0800}) {
    reset(true);
    const uint32_t drops = gEspNowRxMustUnderstandDrops;
    deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, ++id, text("hi"), 0, 1,
                                (uint16_t)(ESPNOW_V4_FLAG_ACK_REQ | bit)));
    char what[120];
    std::snprintf(what, sizeof what, "flag 0x%04X outside the class: ignored, the frame is delivered and ACKed",
                  (unsigned)bit);
    check(what, calls("v4h_text") == 1 && gAcksSent == 1 && gEspNowRxMustUnderstandDrops == drops);
  }

  // Routed: the final receiver checks the inner frame when it re-enters.
  reset(true);
  {
    const uint32_t drops = gEspNowRxMustUnderstandDrops;
    const Frame inner = sealedFrame(gSessB, ESPNOW_V4_TYPE_TIME_SYNC, gPeerB, 760, Frame(16, 0x5A), 0, 1, 0x1000);
    deliver(gRelayR, envelope(gRelayR, inner, 760));
    check("a routed inner frame with flag 0x1000: the envelope is taken, the inner frame dropped at the final "
          "receiver and counted",
          calls("v4hTimeSync") == 0 && gEspNowRxMustUnderstandDrops == drops + 1);
  }
  // A relay forwards an envelope for someone else without looking inside it.
  reset(true);
  {
    const Frame inner = sealedFrame(gSessB, ESPNOW_V4_TYPE_TIME_SYNC, gPeerB, 770, Frame(16, 0x5A), 0, 1, 0x1000);
    V4PayloadRelayData env{};
    std::memcpy(env.finalDst, gFar, 6);
    env.hopsUsed = 1;
    Frame body(sizeof env + inner.size());
    std::memcpy(body.data(), &env, sizeof env);
    std::memcpy(body.data() + sizeof env, inner.data(), inner.size());
    const uint32_t drops = gEspNowRxMustUnderstandDrops;
    gRouteNext = gRelayR2;
    deliver(gRelayR, groupFrame(ESPNOW_V4_TYPE_RELAY_DATA, gRelayR, 770, body, 2));
    check("a relay forwards an envelope whose inner frame carries 0x1000, unopened and uncounted",
          gRelayForwards == 1 && gEspNowRxMustUnderstandDrops == drops);
  }

  // One line a minute for the drops.
  reset(true);
  gNowMs += 120000;
  const unsigned warnsBefore = gWarns;
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 780, text("a"), 0, 1, 0x2000));
  deliver(gPeerB, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, 781, text("b"), 0, 1, 0x2000));
  check("two drops inside a minute print one warning", gWarns == warnsBefore + 1 &&
        gLastWarn.find("must be understood") != std::string::npos);
#else
  check("the tree has the must-understand header flag drop (gEspNowRxMustUnderstandDrops)", false);
#endif
}

// The relay hop rule (1.0, back-capture design W1): a hop that only passes an
// envelope on checks the envelope and the inner header (magic, version,
// header length), ttl, hop count and route, and refuses only an exact list of
// the 1.0 link-local opcodes. Any other inner opcode goes on, also one this
// firmware does not know (opcodes added after 1.0 still cross a 1.0 relay);
// the final destination keeps its own gate and refuses what it does not take
// routed, counting routedDropPolicy.
static Frame envelopeTo(const uint8_t* finalDst, const uint8_t* relay, const Frame& inner, uint32_t msgId,
                        uint8_t ttl = 2) {
  V4PayloadRelayData env{};
  std::memcpy(env.finalDst, finalDst, 6);
  env.hopsUsed = 1;
  Frame body(sizeof env + inner.size());
  std::memcpy(body.data(), &env, sizeof env);
  std::memcpy(body.data() + sizeof env, inner.data(), inner.size());
  return groupFrame(ESPNOW_V4_TYPE_RELAY_DATA, relay, msgId, body, ttl);
}

static void testRelayHopOpcodeRule() {
  uint32_t id = 3000;
  // Opcodes no 1.0 row takes routed: the earmarked back-capture numbers, free
  // and user-defined ones, and 1.0 opcodes the destination refuses routed.
  const uint8_t passOn[] = { 82, 85, 86, 156, 157, 158, 159, 190, 200, 255,
                             ESPNOW_V4_TYPE_FS_LIST_REQ, ESPNOW_V4_TYPE_FILE_DATA, ESPNOW_V4_TYPE_STREAM,
                             ESPNOW_V4_TYPE_SESSION_UNKNOWN };
  for (uint8_t type : passOn) {
    reset(true);
    gRouteNext = gRelayR2;
    const Frame inner = sealedFrame(gSessB, type, gPeerB, ++id, Frame(24, 0x5A), 0, 1, 0);
    deliver(gRelayR, envelopeTo(gFar, gRelayR, inner, id));
    char what[160];
    std::snprintf(what, sizeof what, "a hop passes inner opcode %u on, unopened (FAILS WITHOUT THE W1 RULE)",
                  (unsigned)type);
    check(what, gRelayForwards == 1 && gRelayMetrics.routedForwards == 1 && gRelayMetrics.routedDropPolicy == 0 &&
                gCalls.empty() && gUnwrapCalls == 0);
  }
  // The exact 1.0 link-local list: refused at the hop and counted.
  const uint8_t linkLocal[] = {
    ESPNOW_V4_TYPE_RELAY_DATA, ESPNOW_V4_TYPE_HEARTBEAT, ESPNOW_V4_TYPE_PEER_LIST,
    ESPNOW_V4_TYPE_TOPO_REQ, ESPNOW_V4_TYPE_TOPO_START, ESPNOW_V4_TYPE_TOPO_PEER,
    ESPNOW_V4_TYPE_PAIR_BEACON, ESPNOW_V4_TYPE_PAIR_REQUEST, ESPNOW_V4_TYPE_PAIR_ACCEPT, ESPNOW_V4_TYPE_PAIR_REJECT,
    ESPNOW_V4_TYPE_BOND_HEARTBEAT, ESPNOW_V4_TYPE_BOND_CAP_REQ, ESPNOW_V4_TYPE_BOND_CAP_RESP,
    ESPNOW_V4_TYPE_MANIFEST_REQ, ESPNOW_V4_TYPE_SETTINGS_REQ, ESPNOW_V4_TYPE_BOND_STATUS_REQ,
    ESPNOW_V4_TYPE_BOND_STATUS_RESP, ESPNOW_V4_TYPE_SCHEMA_REQ, ESPNOW_V4_TYPE_BOND_STREAM_CTRL,
    ESPNOW_V4_TYPE_BOND_SENSOR_DATA, ESPNOW_V4_TYPE_BOND_POWER_STATUS, ESPNOW_V4_TYPE_BOND_EVENT_PUSH,
    ESPNOW_V4_TYPE_BOND_REQUEST, ESPNOW_V4_TYPE_BOND_NOTICE, ESPNOW_V4_TYPE_BOND_SENSOR_STATUS };
  for (uint8_t type : linkLocal) {
    reset(true);
    gRouteNext = gRelayR2;
    const Frame inner = plainFrame(type, gPeerB, ++id, Frame(24, 0x5A));
    deliver(gRelayR, envelopeTo(gFar, gRelayR, inner, id));
    char what[160];
    std::snprintf(what, sizeof what, "a hop refuses link-local inner opcode %u and counts it", (unsigned)type);
    check(what, gRelayForwards == 0 && gRelayMetrics.routedDropPolicy == 1 && gCalls.empty());
  }
  // Neither range is refused whole: 181 (earmarked bond number) and 186-189
  // (free bond numbers) go on.
  for (uint8_t type : { (uint8_t)181, (uint8_t)186, (uint8_t)189, (uint8_t)37, (uint8_t)43 }) {
    reset(true);
    gRouteNext = gRelayR2;
    ++id;
    deliver(gRelayR, envelopeTo(gFar, gRelayR, plainFrame(type, gPeerB, id, Frame(8, 0)), id));
    char what[120];
    std::snprintf(what, sizeof what, "opcode %u (not a 1.0 link-local opcode) is not refused by range", (unsigned)type);
    check(what, gRelayForwards == 1 && gRelayMetrics.routedDropPolicy == 0);
  }
  // The hop keeps its other checks for an unknown inner opcode.
  reset(true);
  gRouteNext = gRelayR2;
  ++id;
  deliver(gRelayR, envelopeTo(gFar, gRelayR, sealedFrame(gSessB, 156, gPeerB, id, Frame(24, 1), 0, 1, 0), id, 1));
  check("an unknown inner opcode with ttl 1 at a hop: dropped for ttl", gRelayForwards == 0 &&
        gRelayMetrics.routedDropTtl == 1);
  reset(true);
  ++id;
  deliver(gRelayR, envelopeTo(gFar, gRelayR, sealedFrame(gSessB, 156, gPeerB, id, Frame(24, 1), 0, 1, 0), id));
  check("an unknown inner opcode with no route: dropped as no route", gRelayForwards == 0 &&
        gRelayMetrics.routedDropNoRoute == 1);
  reset(true);
  gRouteNext = gRelayR2;
  gSettings.meshRelay = false;
  ++id;
  deliver(gRelayR, envelopeTo(gFar, gRelayR, sealedFrame(gSessB, 156, gPeerB, id, Frame(24, 1), 0, 1, 0), id));
  check("relaying switched off: an unknown inner opcode is not passed on", gRelayForwards == 0 &&
        gRelayMetrics.routedDropPolicy == 1);
  reset(true);
  gRouteNext = gRelayR2;
  {
    Frame inner = sealedFrame(gSessB, 156, gPeerB, ++id, Frame(24, 1), 0, 1, 0);
    inner[2] = 5;  // the version byte (magic at 0-1)
    deliver(gRelayR, envelopeTo(gFar, gRelayR, inner, id));
  }
  check("an inner frame of another version is refused at the hop", gRelayForwards == 0 &&
        gRelayMetrics.routedDropPolicy == 1);
  reset(true);
  gRouteNext = gRelayR2;
  {
    Frame inner = sealedFrame(gSessB, 156, gPeerB, ++id, Frame(24, 1), 0, 1, 0);
    inner[0] ^= 0xFF;  // the magic
    deliver(gRelayR, envelopeTo(gFar, gRelayR, inner, id));
  }
  check("an inner frame with a wrong magic is refused at the hop", gRelayForwards == 0 &&
        gRelayMetrics.routedDropPolicy == 1);
  // The final destination keeps its gate: an opcode it does not take routed
  // is refused and counted, never handed to a handler.
  for (uint8_t type : { (uint8_t)156, (uint8_t)ESPNOW_V4_TYPE_FILE_DATA, (uint8_t)ESPNOW_V4_TYPE_HEARTBEAT }) {
    reset(true);
    ++id;
    deliver(gRelayR, envelope(gRelayR, sealedFrame(gSessB, type, gPeerB, id, Frame(24, 1), 0, 1, 0), id));
    char what[160];
    std::snprintf(what, sizeof what, "at the final destination inner opcode %u is refused and counted (routedDropPolicy)",
                  (unsigned)type);
    check(what, gCalls.empty() && gRelayMetrics.routedDelivered == 0 && gRelayMetrics.routedDropPolicy == 1 &&
                gRelayForwards == 0);
  }
  // ... and still delivers what it takes routed.
  reset(true);
  ++id;
  deliver(gRelayR, envelope(gRelayR, sealedFrame(gSessB, ESPNOW_V4_TYPE_TEXT, gPeerB, id, text("hi")), id));
  check("at the final destination a routed TEXT is delivered", calls("v4h_text") == 1 &&
        gRelayMetrics.routedDelivered == 1);
}

int main() {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // keep the case lines if a pre-fix tree aborts
  testMeshIdGate();
  testForgedPlaintextDoesNotShadowSealed();
  testAuthenticatedFramesDoNotShadowEachOther();
  testSealedRetriesStillSuppressed();
  testAnswersAreDeduplicated();
  testFloodLoopSuppression();
  testRelayedLoopSuppression();
  testTimeAndSensorFramesThroughRelay();
  testGroupTagIs16Bytes();
  testFragmentIndexConsistency();
  testHeartbeatPolicy();
  testSlotCreation();
  testUnpairReleasesMetadata();
  testMetadataTableFull();
  testReplayedHeartbeatFromInventedSource();
  testUnknownSessionAsksForHint();
  testMustUnderstandFlags();
  testRelayHopOpcodeRule();
  std::printf("espnow_rx_frontend: %u failure%s\n", gFailures, gFailures == 1 ? "" : "s");
  return gFailures ? 1 : 0;
}
