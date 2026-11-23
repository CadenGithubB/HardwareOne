// Host harness: the production KEY_EX_HELLO receive handler (v4hKeyExHello with
// its HMAC check, its accepted-HELLO rate limit and its CONFIRM builder) from
// System_ESPNow_Handlers_Crypto.cpp, on top of the production peer-identity
// cache and peerIdentityPersist from System_ESPNow_Identity.cpp.
//
// test_espnow_keyex_hello.py splices the production code in at the markers.
// Doubles: settable millis() and time(), a log, an outbox for the frames the
// handler sends, a counting peer-file writer that can be made to fail, a
// pairing window, two meshes with fixed bootstrap keys and a deterministic
// keyed digest in place of HMAC-SHA256 (a library call, not logic under
// test). No radio, no filesystem, no hardware.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "System_MacText.h"  // production MAC text helpers (copied beside this unit)
#include "System_ClockCore.h"  // production clock rules: is a time a real date (copied beside this unit)
#include "System_ESPNow_RevokedCore.h"  // production rule A and keyId (vendored ArduinoJson on the path)
#include <set>

// ---- Production wire types: flags, header, KEY_EX payloads -------------------
// INSERT_WIRE

// ---- Production identity records, mesh keys, RX context ----------------------
struct esp_now_recv_info {
  const uint8_t* src_addr;
  const uint8_t* des_addr;
};
// INSERT_TYPES

// ---- Platform doubles ----------------------------------------------------------
static uint32_t gNowMs = 5000;
static uint32_t millis() { return gNowMs; }
static time_t gNowSec = 1800000000;  // a set clock; 0 = not set
static time_t hostTime(time_t*) { return gNowSec; }
#define time hostTime  // the production code calls time(nullptr)

static std::vector<std::string> gLog;
__attribute__((format(printf, 2, 3)))
static void noteLog(const char* level, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  gLog.push_back(std::string(level) + ": " + buf);
}
#define ERROR_ESPNOWF(...) noteLog("ERROR", __VA_ARGS__)
#define WARN_ESPNOWF(...)  noteLog("WARN", __VA_ARGS__)
#define INFO_ESPNOWF(...)  noteLog("INFO", __VA_ARGS__)
#define DEBUG_ESPNOWF(...) noteLog("DEBUG", __VA_ARGS__)
#define EXT_RAM_BSS_ATTR

static unsigned gEvents = 0;
static void logSystemEvent(const char*, const char*, ...) { ++gEvents; }

// What a stored identity that is new for its MAC tells the registry: the room
// and tags accepted for that MAC do not carry over (the clearing itself is run
// by test_espnow_group_membership.py).
static unsigned gAcceptClears = 0;
static uint8_t gAcceptClearMac[6] = {};
static void espnowGroupAcceptanceClear(const uint8_t mac[6], const char*) {
  ++gAcceptClears;
  std::memcpy(gAcceptClearMac, mac, 6);
}

// Two enabled meshes with different fingerprints and bootstrap keys.
struct Settings {
  static constexpr uint8_t N_MESHES = 4;
  struct Mesh { bool enabled; uint16_t fingerprint; };
  Mesh meshes[N_MESHES];
};
static Settings gSettings = {{{true, 0x1234}, {true, 0x4321}, {false, 0}, {false, 0}}};
static MeshDerivedKeys gMeshKeys[2];
static const MeshDerivedKeys* meshKeysFindByFingerprint(uint16_t fp) {
  for (const auto& k : gMeshKeys)
    if (k.valid && k.fingerprint == fp) return &k;
  return nullptr;
}

// Keyed digest over the three chunks: any changed byte or key gives another tag.
static bool espnowCryptoHmacSha256(uint8_t out[32], const uint8_t* key, size_t keyLen,
                                   const uint8_t* in1, size_t len1, const uint8_t* in2, size_t len2,
                                   const uint8_t* in3, size_t len3) {
  uint64_t h = 0xcbf29ce484222325ULL;
  auto mix = [&h](const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    h ^= n; h *= 0x100000001b3ULL;
  };
  mix(key, keyLen); mix(in1, len1); mix(in2, len2); mix(in3, len3);
  for (int i = 0; i < 32; i++) {
    h ^= (uint64_t)i; h *= 0x100000001b3ULL;
    out[i] = (uint8_t)(h >> 29);
  }
  return true;
}

// The local `espnowpairsecure` re-key window: one use.
static bool gWindowOpen = false;
static unsigned gWindowUses = 0;
static bool espnowConsumePairingWindow(const uint8_t*) {
  if (!gWindowOpen) return false;
  gWindowOpen = false;
  ++gWindowUses;
  return true;
}

static EspNowIdentity gSelf;
static const uint8_t kSelfMac[6] = {0x02, 0, 0, 0, 0, 0x01};
static const EspNowIdentity& espnowIdentityGet() { return gSelf; }
static int espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, kSelfMac, 6); return 0; }
// The radio peer table: what ensureUnencryptedPeer added and esp_now_del_peer
// removed (a refusal must not leave a lasting radio peer).
static std::set<std::string> gRadioPeers;
static unsigned gRadioAdds = 0, gRadioDels = 0;
static std::string macKey(const uint8_t* m) { return std::string(reinterpret_cast<const char*>(m), 6); }
static bool esp_now_is_peer_exist(const uint8_t* m) { return gRadioPeers.count(macKey(m)) != 0; }
static int esp_now_del_peer(const uint8_t* m) { ++gRadioDels; gRadioPeers.erase(macKey(m)); return 0; }
static bool ensureUnencryptedPeer(const uint8_t* m) {
  if (gRadioPeers.insert(macKey(m)).second) ++gRadioAdds;
  return true;
}
static bool logCooldownOk(uint32_t& lastMs, uint32_t windowMs) {
  if (lastMs != 0 && (uint32_t)(millis() - lastMs) < windowMs) return false;
  lastMs = millis() ? millis() : 1;
  return true;
}

// Rule A's inputs (production: espnowKeyExAdmit in System_ESPNow.cpp gathers
// them from the registry, the identity cache and the revoked list; the rule
// itself is the production espnowrevoke::admit). By default every MAC is a
// registry entry inside its first-key window; a case takes MACs out, closes
// the window, revokes a MAC or a key, or locks the list.
static std::set<std::string> gNotInRegistry, gRevokedMacs, gRevokedKeys;
static bool gFirstKeyWindow = true, gListLocked = false;
static unsigned gBound = 0, gRollbacks = 0;
struct PeerIdentity;
static const PeerIdentity* peerIdentityFindByMac(const uint8_t mac[6]);
static espnowrevoke::Admit espnowKeyExAdmit(const uint8_t mac[6], const uint8_t pub[32]);
static void espnowFirstKeyBound(const uint8_t*) { ++gBound; }
static void espnowPairRollbackRevoked(const uint8_t*) { ++gRollbacks; }

struct Sent { uint8_t dst[6]; uint8_t type; std::vector<uint8_t> payload; };
static std::vector<Sent> gOutbox;
static uint32_t gNextMsgId = 100;
static uint32_t generateMessageId() { return gNextMsgId++; }
static bool v4_send_frame(const uint8_t* dst, uint8_t type, uint16_t, uint32_t,
                          const uint8_t* payload, uint16_t payloadLen, uint8_t) {
  Sent s;
  std::memcpy(s.dst, dst, 6);
  s.type = type;
  s.payload.assign(payload, payload + payloadLen);
  gOutbox.push_back(s);
  return true;
}

// The peer-file writer: counts, keeps the last record handed to it, can fail.
static unsigned gWrites = 0;
static bool gWriteFails = false;
static PeerIdentity gLastWritten;
static bool writePeerIdentityFile(const PeerIdentity& p) {
  if (gWriteFails) return false;
  ++gWrites;
  gLastWritten = p;
  return true;
}

// What the KEY_EX_REPLY handler calls after it stored the identity: no
// session and nothing queued here, so it sends its CONFIRM and stops.
static void espnowHandshakeBackoffClear(const uint8_t*) {}
enum : uint8_t { SESSION_FREE = 0, SESSION_ACTIVE = 2 };
struct SessionState { uint8_t state; };
static const SessionState* sessionFindByPeer(const uint8_t*, uint8_t) { return nullptr; }
static void pendingFrameDrainForPeer(const uint8_t*) {}
static bool pendingFrameHasForPeer(const uint8_t*) { return false; }
static bool espnowSessionOpenInitiate(const uint8_t*, const char*, bool* = nullptr) { return true; }

// ---- Production: Identity.cpp peer cache + peerIdentityPersist ---------------
// INSERT_IDENTITY

static espnowrevoke::Admit espnowKeyExAdmit(const uint8_t mac[6], const uint8_t pub[32]) {
  espnowrevoke::AdmitInputs in = {};
  in.revoked = gRevokedMacs.count(macKey(mac)) || gRevokedKeys.count(std::string(reinterpret_cast<const char*>(pub), 32));
  in.inRegistry = !gNotInRegistry.count(macKey(mac));
  const PeerIdentity* id = peerIdentityFindByMac(mac);
  in.hasIdentity = id != nullptr;
  in.sameKey = id && std::memcmp(id->longTermPub, pub, 32) == 0;
  in.listLocked = gListLocked;
  in.firstKeyOpen = gFirstKeyWindow;
  return espnowrevoke::admit(in);
}

// ---- Production: Handlers_Crypto.cpp KEY_EX_HELLO path -------------------------
// INSERT_KEYEX
#undef time
// A tree without the unpair call-off: the unpair did nothing to the key exchange.
#ifndef HW1_HAS_KEYEX_CANCEL
static void espnowKeyExCancel(const uint8_t*) {}
#endif

// ---- Test driver ------------------------------------------------------------------
// The interval the cases are written for; tied to the production constant
// where the source has the limit (HW1_HAS_HELLO_LIMIT, set by the driver).
constexpr uint32_t kLimitMs = 1000;
#ifdef HW1_HAS_HELLO_LIMIT
static_assert(kKeyExHelloMinIntervalMs == kLimitMs, "cases assume this interval");
static_assert(kKeyExHelloSlots == kPeerSlots, "one limiter slot per peer-identity slot");
#endif

static unsigned gFailures = 0;
static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) gFailures++;
}

static unsigned sentOfType(uint8_t type) {
  unsigned n = 0;
  for (const auto& s : gOutbox) n += s.type == type;
  return n;
}
static unsigned replies() { return sentOfType(ESPNOW_V4_TYPE_KEY_EX_REPLY); }
static unsigned confirms() { return sentOfType(ESPNOW_V4_TYPE_KEY_EX_CONFIRM); }

// A device that sends us HELLOs: locally administered test MAC, key byte `k`.
struct Peer { uint8_t mac[6]; uint8_t pub[32]; };
static Peer peer(uint8_t n, uint8_t k) {
  Peer p;
  const uint8_t mac[6] = {0x02, 0, 0, 0, 0x10, n};
  std::memcpy(p.mac, mac, 6);
  std::memset(p.pub, k, 32);
  return p;
}

// Deliver a KEY_EX_HELLO from `p` on mesh `mesh` the way the dispatcher does.
// Every delivery has a new header msgId, as a replayed copy would to pass the
// duplicate ring. `forge` breaks the HMAC.
static void hello(const Peer& p, unsigned mesh = 0, bool forge = false) {
  V4PayloadKeyExHello msg{};
  msg.meshFingerprint = gSettings.meshes[mesh].fingerprint;
  std::memcpy(msg.senderMac, p.mac, 6);
  std::memcpy(msg.senderPubEd25519, p.pub, 32);
  computeKeyExHmac(msg.meshFingerprint, msg.senderMac, msg.senderPubEd25519,
                   gMeshKeys[mesh].bootstrapKey, msg.hmac);
  if (forge) msg.hmac[0] ^= 0x01;
  EspNowV4Header h{};
  h.type = ESPNOW_V4_TYPE_KEY_EX_HELLO;
  h.flags = ESPNOW_V4_FLAG_ACK_REQ;
  h.msgId = generateMessageId();
  std::memcpy(h.origin, p.mac, 6);
  esp_now_recv_info ri{p.mac, kSelfMac};
  V4RxCtx ctx{};
  ctx.recv_info = &ri;
  ctx.h = &h;
  ctx.payload = reinterpret_cast<const uint8_t*>(&msg);
  ctx.payloadLen = sizeof msg;
  v4hKeyExHello(ctx);
}

static bool storedKeyIs(const Peer& p, uint8_t k) {
  const PeerIdentity* id = peerIdentityFindByMac(p.mac);
  if (!id) return false;
  for (uint8_t b : id->longTermPub)
    if (b != k) return false;
  return true;
}

// Deliver a KEY_EX_REPLY from `p` (the answer to our HELLO) on mesh `mesh`.
static void replyFrom(const Peer& p, unsigned mesh = 0) {
  V4PayloadKeyExReply msg{};
  msg.meshFingerprint = gSettings.meshes[mesh].fingerprint;
  std::memcpy(msg.responderMac, p.mac, 6);
  std::memcpy(msg.responderPubEd25519, p.pub, 32);
  computeKeyExHmac(msg.meshFingerprint, msg.responderMac, msg.responderPubEd25519,
                   gMeshKeys[mesh].bootstrapKey, msg.hmac);
  EspNowV4Header h{};
  h.type = ESPNOW_V4_TYPE_KEY_EX_REPLY;
  h.flags = ESPNOW_V4_FLAG_ACK_REQ;
  h.msgId = generateMessageId();
  std::memcpy(h.origin, p.mac, 6);
  esp_now_recv_info ri{p.mac, kSelfMac};
  V4RxCtx ctx{};
  ctx.recv_info = &ri;
  ctx.h = &h;
  ctx.payload = reinterpret_cast<const uint8_t*>(&msg);
  ctx.payloadLen = sizeof msg;
  v4hKeyExReply(ctx);
}

// Empty peer cache, limiter, outbox, log and counters; the clock moves on.
static void reset() {
  for (auto& p : gPeerIdentities) p = PeerIdentity{};
#ifdef HW1_HAS_HELLO_LIMIT
  std::memset(gKeyExHelloSeen, 0, sizeof gKeyExHelloSeen);
#endif
  gOutbox.clear();
  gLog.clear();
  gWrites = 0;
  gWriteFails = false;
  gWindowOpen = false;
  gWindowUses = 0;
  gEvents = 0;
  gAcceptClears = 0;
  for (auto& k : gKeyExInFlight) k = KeyExInFlight{};
  gRadioPeers.clear();
  gRadioAdds = gRadioDels = 0;
  gNotInRegistry.clear();
  gRevokedMacs.clear();
  gRevokedKeys.clear();
  gFirstKeyWindow = true;
  gListLocked = false;
  gBound = gRollbacks = 0;
  gNowMs += 100000;
  gNowSec += 100000;
}

int main() {
  for (unsigned i = 0; i < 2; i++) {
    gMeshKeys[i].valid = true;
    gMeshKeys[i].fingerprint = gSettings.meshes[i].fingerprint;
    std::memset(gMeshKeys[i].bootstrapKey, 0x40 + (int)i, 32);
  }
  gSelf.valid = true;
  std::memset(gSelf.pub, 0xEE, 32);

  // K1 — a first HELLO is answered at once and stored.
  reset();
  const Peer a = peer(1, 0xA1);
  hello(a);
  check(replies() == 1 && gWrites == 1 && storedKeyIs(a, 0xA1) && gEvents == 1,
        "K1 a first HELLO gets its REPLY at once: one file write, one 'paired' event (preserved)");
  check(gLastWritten.lastSeenSec == (uint32_t)gNowSec && gLastWritten.bondedAtSec == (uint32_t)gNowSec &&
        gLastWritten.meshId == 0,
        "K1 the stored record carries the mesh and the time (preserved)");

  // K2 — the defect: the same HELLO replayed with new msgIds, ten a second
  // for five seconds.
  const uint32_t t0 = gNowMs;
  const time_t s0 = gNowSec;
  for (int i = 0; i < 50; i++) {
    gNowMs += 100;
    gNowSec = s0 + (time_t)((gNowMs - t0) / 1000);
    hello(a);
  }
  check(gWrites == 1, "K2 fifty replayed HELLOs cause no further file write (FAILS ON BASE: one each)");
  check(replies() >= 2 && replies() - 1 <= (gNowMs - t0) / kLimitMs,
        "K2 and at most one REPLY per interval (FAILS ON BASE: one each)");
  check(gEvents == 1, "K2 no further 'paired' event (preserved)");
  check(gAcceptClears == 1 && std::memcmp(gAcceptClearMac, a.mac, 6) == 0,
        "K2 the first HELLO's new identity cleared the room and tags accepted for that MAC, the replays did not");

  // K3 — the initiator's own retry after a lost REPLY (kKeyExRetryMs later)
  // is answered, without a second write.
  reset();
  hello(a);
  gNowMs += kKeyExRetryMs;
  hello(a);
  gNowMs += kKeyExRetryMs;
  hello(a);
  check(replies() == 3, "K3 the HELLO and both of its retries are answered (preserved)");
  check(gWrites == 1, "K3 with one file write in all (FAILS ON BASE: three)");

  // K4 — a changed key. Without the local re-key window it is refused
  // (CONFIRM status 2, nothing written); with the window it replaces the
  // stored identity and that is written.
  reset();
  hello(a);
  const Peer a2 = peer(1, 0xA2);  // same MAC, another key
  gNowMs += kLimitMs;
  hello(a2);
  check(gWrites == 1 && replies() == 1 && confirms() == 1 && storedKeyIs(a, 0xA1) &&
        gOutbox.back().payload.size() == sizeof(V4PayloadKeyExConfirm) &&
        reinterpret_cast<const V4PayloadKeyExConfirm*>(gOutbox.back().payload.data())->status == 2,
        "K4 a changed key without the window: refused with CONFIRM status 2, no write (preserved)");
  check(gAcceptClears == 1, "K4 the refused key leaves the accepted room and tags alone");
  gWindowOpen = true;
  gNowMs += kLimitMs;
  hello(a2);
  check(gWrites == 2 && replies() == 2 && storedKeyIs(a, 0xA2) && gWindowUses == 1,
        "K4 a changed key inside the window is stored: a HELLO with a changed key still writes (preserved)");
  check(gAcceptClears == 2, "K4 the replaced identity does not inherit the accepted room and tags");

  // K5 — a HELLO dropped by the rate limit does not use up the re-key window.
  reset();
  hello(a);
  gWindowOpen = true;
  gNowMs += 100;
  hello(a2);
  check(gWindowOpen && gWindowUses == 0 && storedKeyIs(a, 0xA1) && gWrites == 1 && replies() == 1 &&
        confirms() == 0,
        "K5 a second HELLO 100 ms after an answered one is dropped: no reply, window untouched (FAILS ON BASE)");
  gNowMs += kLimitMs - 100;
  hello(a2);
  check(!gWindowOpen && gWindowUses == 1 && storedKeyIs(a, 0xA2) && gWrites == 2 && replies() == 2,
        "K5 once the interval has passed the changed key is taken through the window");

  // K6 — the stored lastSeenSec moves forward once an hour at most, and never
  // to the reading of a clock that is not set.
  reset();
  hello(a);
  const time_t paired = gNowSec;
  gNowMs += 5000;
  gNowSec = paired + 3599;
  hello(a);
  check(gWrites == 1 && replies() == 2, "K6 59 min 59 s later: answered, nothing written (FAILS ON BASE)");
  gNowMs += 5000;
  gNowSec = paired + 3600;
  hello(a);
  check(gWrites == 2 && gLastWritten.lastSeenSec == (uint32_t)(paired + 3600) &&
        gLastWritten.bondedAtSec == (uint32_t)paired && replies() == 3 && gEvents == 1,
        "K6 one hour later: lastSeenSec is written once, bondedAtSec kept");
  gNowMs += 5000;
  gNowSec = paired + 3610;
  hello(a);
  gNowMs += 5000;
  gNowSec = 0;
  hello(a);
  gNowMs += 5000;
  gNowSec = 40;
  hello(a);
  check(gWrites == 2 && replies() == 6 &&
        peerIdentityFindByMac(a.mac)->lastSeenSec == (uint32_t)(paired + 3600),
        "K6 then not again, and not with an unset clock (FAILS ON BASE)");

  // K7 — the same peer and key seen on another mesh slot is a change and is written.
  reset();
  hello(a, 0);
  gNowMs += kLimitMs;
  hello(a, 1);
  check(gWrites == 2 && replies() == 2 && peerIdentityFindByMac(a.mac)->meshId == 1,
        "K7 a HELLO on another mesh updates and writes the record (preserved)");

  // K8 — a failed file write: no REPLY, and the cache stays equal to the file,
  // so the next HELLO writes instead of finding the key "unchanged".
  reset();
  gWriteFails = true;
  hello(a);
  check(replies() == 0 && gWrites == 0, "K8 a failed write of a new identity sends no REPLY (preserved)");
  check(peerIdentityFindByMac(a.mac) == nullptr,
        "K8 and leaves no identity in the cache (FAILS ON BASE: cached, never written)");
  gWriteFails = false;
  gNowMs += kLimitMs;
  hello(a);
  check(replies() == 1 && gWrites == 1 && storedKeyIs(a, 0xA1), "K8 the next HELLO stores it and is answered");
  gWriteFails = true;
  gWindowOpen = true;
  gNowMs += kLimitMs;
  hello(a2);
  check(replies() == 1 && gWrites == 1 && storedKeyIs(a, 0xA1),
        "K8 a failed write of a replacement key keeps the stored key in the cache (FAILS ON BASE)");

  // K9 — the limit is per sender MAC: sixteen peers, three rounds 100 ms apart.
  reset();
  for (int round = 0; round < 3; round++) {
    for (uint8_t n = 1; n <= 16; n++) hello(peer(n, (uint8_t)(0x50 + n)));
    gNowMs += 100;
  }
  check(gWrites == 16, "K9 sixteen peers are each stored once (FAILS ON BASE: 48 writes)");
  check(replies() == 16, "K9 and each answered once inside the interval (FAILS ON BASE: 48 replies)");
  gNowMs += kLimitMs;
  for (uint8_t n = 1; n <= 16; n++) hello(peer(n, (uint8_t)(0x50 + n)));
  check(replies() == 32 && gWrites == 16, "K9 after the interval each is answered again, with no write");

  // K10 — a HELLO that fails the HMAC check takes no slot in the limit: the
  // genuine first HELLO right after it is still answered at once.
  reset();
  hello(a, 0, /*forge=*/true);
  check(replies() == 0 && gWrites == 0 && peerIdentityFindByMac(a.mac) == nullptr,
        "K10 a HELLO with a bad HMAC is rejected (preserved)");
  gNowMs += 10;
  hello(a);
  check(replies() == 1 && gWrites == 1, "K10 the genuine HELLO 10 ms later is answered at once");

  // K11 — the interval is measured across the millis() rollover.
  reset();
  gNowMs = 0xFFFFFF00u;
  hello(a);
  gNowMs += 500;  // wraps
  hello(a);
  gNowMs += 500;
  hello(a);
  check(replies() == 2 && gWrites == 1, "K11 one REPLY per interval across the millis() rollover");

  // K12 — one pairing action (espnowpairsecure) can send two HELLOs tens of
  // milliseconds apart for two meshes both devices share: the seed heartbeat's
  // and the command's own. The second is not a copy of the first.
  reset();
  hello(a, 0);
  gNowMs += 30;
  hello(a, 1);
  check(replies() == 2 && gWrites == 2 && peerIdentityFindByMac(a.mac)->meshId == 1,
        "K12 two HELLOs for two meshes 30 ms apart are both answered; the record ends on the second mesh");
  gNowMs += 30;
  hello(a, 1);
  check(replies() == 2 && gWrites == 2, "K12 a copy of the second one inside the interval is still dropped");

  // K13 — finding X4 (round 3): a REPLY to a key exchange that an unpair
  // called off stores nothing, so the unpaired peer's identity (and its
  // folder) does not come back; a new key exchange after it works as before.
  reset();
  const Peer b = peer(7, 0xB7);
  keyExInFlightArm(b.mac, 0, true);           // our automatic HELLO went out
  replyFrom(b);
  check(gWrites == 1 && storedKeyIs(b, 0xB7) && confirms() == 1,
        "K13 a REPLY to our HELLO is stored and confirmed (preserved)");
  check(gAcceptClears == 1 && std::memcmp(gAcceptClearMac, b.mac, 6) == 0,
        "K13 an identity learnt from a REPLY clears the accepted room and tags as one from a HELLO does");
  reset();
  keyExInFlightArm(b.mac, 0, true);           // HELLO out, then the user unpairs b
  espnowKeyExCancel(b.mac);
  replyFrom(b);
  check(gWrites == 0 && peerIdentityFindByMac(b.mac) == nullptr && confirms() == 0,
        "K13 a REPLY to a key exchange an unpair called off stores nothing (FAILS ON BASE: the identity is back)");
  gNowMs += 50;
  const unsigned confirmsBefore = confirms();
  keyExInFlightArm(b.mac, 0, false);          // the user pairs b again (espnowkeyex)
  replyFrom(b);
  check(storedKeyIs(b, 0xB7) && confirms() == confirmsBefore + 1,
        "K13 a new key exchange after the unpair is stored and confirmed as usual");

  // ---- ESP-NOW 1.0 step 3, rule A and the revoked list --------------------------
  auto lastConfirmStatus = []() -> int {
    for (auto it = gOutbox.rbegin(); it != gOutbox.rend(); ++it)
      if (it->type == ESPNOW_V4_TYPE_KEY_EX_CONFIRM)
        return reinterpret_cast<const V4PayloadKeyExConfirm*>(it->payload.data())->status;
    return -1;
  };
  auto logged = [](const char* part) {
    for (const auto& l : gLog)
      if (l.find(part) != std::string::npos) return true;
    return false;
  };

  // K14 — a HELLO from a MAC with no registry entry stores nothing, is
  // answered "not accepted" (status 3) and leaves no radio peer.
  reset();
  const Peer r = peer(9, 0xC9);
  gNotInRegistry.insert(macKey(r.mac));
  hello(r);
  check(gWrites == 0 && peerIdentityFindByMac(r.mac) == nullptr && replies() == 0,
        "K14 a HELLO from a non-registry MAC stores nothing and gets no REPLY (FAILS ON BASE: stored)");
  check(confirms() == 1 && lastConfirmStatus() == espnowrevoke::kConfirmNotAccepted,
        "K14 it is answered KEY_EX_CONFIRM status 3 (not accepted)");
  check(!esp_now_is_peer_exist(r.mac) && gRadioAdds == 1 && gRadioDels == 1,
        "K14 the radio peer for the answer is added, used and deleted: none is left (FAILS ON BASE: one stays)");
  gRadioPeers.insert(macKey(r.mac));   // a radio peer that was there before
  gNowMs += kLimitMs;
  hello(r);
  check(esp_now_is_peer_exist(r.mac) && gRadioDels == 1, "K14 a radio peer that was there before is left alone");
  gNowMs += 100;
  hello(r);
  check(confirms() == 2, "K14 the refusal is rate-limited like any HELLO (one per MAC per second)");

  // K15 — a revoked key is refused under any MAC: nothing stored, no answer
  // at all (G3), and a pairsecure entry waiting for it is rolled back.
  reset();
  const Peer rk = peer(10, 0xD1);
  gRevokedKeys.insert(std::string(reinterpret_cast<const char*>(rk.pub), 32));
  hello(rk);
  const Peer rkOther = peer(11, 0xD1);   // the same key from another MAC
  gNowMs += kLimitMs;
  hello(rkOther);
  check(gWrites == 0 && gOutbox.empty() && peerIdentityFindByMac(rk.mac) == nullptr &&
            peerIdentityFindByMac(rkOther.mac) == nullptr,
        "K15 a revoked key is refused under any MAC: nothing stored, nothing sent (FAILS ON BASE)");
  check(gRollbacks == 2 && gRadioAdds == 0, "K15 the waiting pairsecure entry is rolled back; no radio peer is added");
  check(logged("revoked here"), "K15 the drop is logged");

  // K16 — a revoked MAC with a key that is not listed: the same.
  reset();
  const Peer rm = peer(12, 0xE1);
  gRevokedMacs.insert(macKey(rm.mac));
  hello(rm);
  check(gWrites == 0 && gOutbox.empty(), "K16 a revoked MAC is refused whatever key it presents");

  // K17 — the first-key window (E31): closed, nothing is stored; open, the key
  // is stored, its keyId logged and the window closed.
  reset();
  const Peer w = peer(13, 0xAB);
  gFirstKeyWindow = false;
  hello(w);
  check(gWrites == 0 && lastConfirmStatus() == espnowrevoke::kConfirmNotAccepted,
        "K17 a registry entry whose first-key window is over stores no first key (status 3)");
  gFirstKeyWindow = true;
  gNowMs += kLimitMs;
  hello(w);
  check(gWrites == 1 && storedKeyIs(w, 0xAB) && gBound == 1 && logged("keyId abababababababab"),
        "K17 inside the window the first key is stored, its keyId logged and the window closed");
  gFirstKeyWindow = false;
  gNowMs += kLimitMs;
  hello(w);
  check(replies() == 2, "K17 a key already stored is answered after the window (no window needed)");

  // K18 — a REPLY from a MAC with no registry entry stores nothing either.
  reset();
  const Peer rr = peer(14, 0x77);
  gNotInRegistry.insert(macKey(rr.mac));
  keyExInFlightArm(rr.mac, 0, false);
  replyFrom(rr);
  check(gWrites == 0 && peerIdentityFindByMac(rr.mac) == nullptr &&
            lastConfirmStatus() == espnowrevoke::kConfirmNotAccepted && !esp_now_is_peer_exist(rr.mac),
        "K18 a REPLY from a non-registry MAC: nothing stored, status 3, no radio peer left");
  reset();
  gRevokedKeys.insert(std::string(reinterpret_cast<const char*>(rr.pub), 32));
  keyExInFlightArm(rr.mac, 0, false);
  replyFrom(rr);
  check(gWrites == 0 && gOutbox.empty() && gRollbacks == 1,
        "K18 a REPLY with a revoked key: nothing stored or sent; the pairing is rolled back");

  // K19 — fail closed: while revoked.json cannot be read no new key is stored.
  reset();
  gListLocked = true;
  hello(a);
  check(gWrites == 0 && lastConfirmStatus() == espnowrevoke::kConfirmNotAccepted,
        "K19 a locked revoked list refuses a first key");

  std::printf("%u failure(s)\n", gFailures);
  return gFailures ? 1 : 0;
}
