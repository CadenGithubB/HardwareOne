// Host harness: production SESSION_REKEY initiator, responder and retry sweep
// from System_ESPNow_Handlers_Crypto.cpp (sendRekey, runDeferredRekey,
// espnowRekeyInitiate, rekeyRetrySweep and the in-flight table) together with
// the Sessions.cpp session lifecycle and the espnow tick's REKEY auto-trigger
// (System_ESPNow.cpp step 1e), driven against modelled peers.
// Doubles: a settable clock, captured log lines, a counting lock, calloc-backed
// ps_alloc, a capturing radio and deterministic stand-in crypto (a commutative
// "X25519", an XOR KDF and a recomputable "Ed25519"); libsodium is a library
// call, not logic under test. No radio, no hardware.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- Deadline owner (System_ESPNow_Deadline.h, when the tree has it) ---------
// INSERT_DEADLINE

// ---- Production types: session record, REKEY wire payload, identities -------
// INSERT_TYPES
static_assert(kRekeyTxFramesThreshold > 0 && kRekeyAgeThresholdMs > kRekeyMinIntervalMs &&
              kRekeyPrevKeysWindowMs > 0 && kSessionSendTimeoutsBeforeReestablish >= 1,
              "header constants referenced");

// ---- Platform doubles ---------------------------------------------------------
static uint32_t gNow = 1000;
static uint32_t millis() { return gNow; }

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
#define DEBUG_ESPNOW_CORE 0
#define DEBUGF(flag, ...)  noteLog("DEBUG", __VA_ARGS__)
#define DEBUG_ESPNOWF(...) noteLog("DEBUG", __VA_ARGS__)

// No file transfer in flight: the auto-trigger's mid-transfer deferral stays off.
static bool espnowFileTransferActiveWithPeer(const uint8_t*) { return false; }

enum class AllocPref { PreferPSRAM };
static const char* gFailAllocTag = nullptr;   // ps_alloc returns null for this tag
static void* ps_alloc(size_t n, AllocPref, const char* tag) {
  if (gFailAllocTag && tag && std::strcmp(tag, gFailAllocTag) == 0) return nullptr;
  return std::calloc(1, n);
}
static void sodium_memzero(void* p, size_t n) { std::memset(p, 0, n); }
static int sodium_is_zero(const unsigned char* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return 0;
  return 1;
}

static unsigned gGuardDepth = 0;
struct EspNowTxGuard {
  bool held;
  explicit EspNowTxGuard(const char* = nullptr) : held(true) { ++gGuardDepth; }
  ~EspNowTxGuard() { --gGuardDepth; }
  EspNowTxGuard(const EspNowTxGuard&) = delete;
  EspNowTxGuard& operator=(const EspNowTxGuard&) = delete;
};

static bool logCooldownOk(uint32_t& lastMs, uint32_t windowMs) {
  if (lastMs != 0 && (uint32_t)(gNow - lastMs) < windowMs) return false;
  lastMs = gNow;
  return true;
}
static unsigned gDrains = 0, gDrainsUnderGuard = 0;
static void pendingFrameDrainForPeer(const uint8_t*) {
  ++gDrains;
  if (gGuardDepth) ++gDrainsUnderGuard;
}

struct EspNowState { bool initialized; };
static EspNowState gStateObj{true};
static EspNowState* gEspNow = &gStateObj;

static EspNowIdentity gSelf;
static const EspNowIdentity& espnowIdentityGet() { return gSelf; }
static const uint8_t gSelfMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0A};
static int espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, gSelfMac, 6); return 0; }

constexpr unsigned kPeers = 16;
static PeerIdentity gPeerTable[kPeers];
static const PeerIdentity* peerIdentityFindByMac(const uint8_t mac[6]) {
  for (const auto& p : gPeerTable)
    if (p.valid && std::memcmp(mac, p.mac, 6) == 0) return &p;
  return nullptr;
}

static uint32_t gNextMsgId = 100;
static uint32_t generateMessageId() { return gNextMsgId++; }

struct SentFrame {
  uint8_t dst[6];
  uint8_t type;
  uint16_t flags;
  uint32_t msgId;
  std::vector<uint8_t> payload;
  uint8_t ttl;
};
static std::vector<SentFrame> gSent;
static bool gSendFails = false;
static unsigned gSendsUnderGuard = 0;
static bool v4_send_frame(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                          const uint8_t* payload, uint16_t payloadLen, uint8_t ttl) {
  if (gGuardDepth) ++gSendsUnderGuard;
  if (gSendFails) return false;
  SentFrame f;
  std::memcpy(f.dst, dst, 6);
  f.type = type;
  f.flags = flags;
  f.msgId = msgId;
  f.payload.assign(payload, payload + payloadLen);
  f.ttl = ttl;
  gSent.push_back(f);
  return true;
}

// ---- Deterministic stand-in crypto -------------------------------------------
// "X25519": pub = sec ^ 0x5A, shared(sec, pub) = (sec ^ 0x5A) ^ pub ^ 0x33,
// which is commutative, so both ends derive the same secret.
// "Ed25519": sec = seed(32) || pub(32); sig[i] = m[i] ^ pub[i % 32].
static uint32_t gSeed = 7;
static void espnowCryptoRandomBytes(uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    gSeed = gSeed * 1103515245u + 12345u;
    out[i] = (uint8_t)(gSeed >> 16);
  }
}
static bool espnowCryptoX25519Keygen(uint8_t pub[32], uint8_t sec[32]) {
  espnowCryptoRandomBytes(sec, 32);
  for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(sec[i] ^ 0x5A);
  return true;
}
static bool espnowCryptoX25519Shared(uint8_t out[32], const uint8_t sec[32], const uint8_t pub[32]) {
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)((sec[i] ^ 0x5A) ^ pub[i] ^ 0x33);
  return true;
}
static bool espnowCryptoKdfSubkey(uint8_t out[32], const uint8_t master[32], uint64_t id, const char* ctx) {
  size_t n = std::strlen(ctx);
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(master[i] ^ (uint8_t)id ^ (uint8_t)ctx[i % n]);
  return true;
}
static bool espnowCryptoEd25519Sign(uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t sec[64]) {
  for (int i = 0; i < 64; i++) sig[i] = (uint8_t)(((size_t)i < n ? m[i] : 0) ^ sec[32 + (i % 32)]);
  return true;
}
static bool espnowCryptoEd25519Verify(const uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t pub[32]) {
  for (int i = 0; i < 64; i++)
    if (sig[i] != (uint8_t)(((size_t)i < n ? m[i] : 0) ^ pub[i % 32])) return false;
  return true;
}

// ---- Production: Sessions.cpp lifecycle ----------------------------------------
// INSERT_SESSIONS

// ---- Production: Handlers_Crypto.cpp REKEY internals (anonymous namespace) ----
namespace {
// INSERT_CRYPTO_INTERNAL
}  // namespace

// ---- Production: Handlers_Crypto.cpp REKEY entry points ------------------------
// INSERT_CRYPTO_PUBLIC

// ---- Production: System_ESPNow.cpp step 1e, the espnow tick's REKEY auto-trigger -
static void autoRekeyTick() {
// INSERT_AUTO_TRIGGER
}

// ---- Modelled peers ---------------------------------------------------------------
struct PeerSide {
  uint8_t mac[6];
  uint8_t idSec[64];
  uint8_t ephPub[32], ephSec[32];
  uint8_t keyTx[32], keyRx[32];
  uint8_t myDirection;
};
static PeerSide gB[kPeers];

static void makeIdentity(uint8_t pub[32], uint8_t sec[64]) {
  espnowCryptoRandomBytes(sec, 32);
  espnowCryptoRandomBytes(pub, 32);
  std::memcpy(sec + 32, pub, 32);
}

static void peerDerive(PeerSide& b, const uint8_t otherEphPub[32]) {
  uint8_t shared[32], kAtoB[32], kBtoA[32];
  espnowCryptoX25519Shared(shared, b.ephSec, otherEphPub);
  espnowCryptoKdfSubkey(kAtoB, shared, 1, "esp-AtoB");
  espnowCryptoKdfSubkey(kBtoA, shared, 2, "esp-BtoA");
  std::memcpy(b.keyTx, b.myDirection == 0 ? kAtoB : kBtoA, 32);
  std::memcpy(b.keyRx, b.myDirection == 0 ? kBtoA : kAtoB, 32);
}

// The peer's own signed REKEY with a fresh eph (its initiation or its reply).
static V4PayloadSessionRekey peerRekey(PeerSide& b, uint16_t sessionId, uint32_t bTxSeq) {
  espnowCryptoX25519Keygen(b.ephPub, b.ephSec);
  V4PayloadSessionRekey rk = {};
  rk.sessionId = sessionId;
  std::memcpy(rk.senderMac, b.mac, 6);
  std::memcpy(rk.receiverMac, gSelfMac, 6);
  std::memcpy(rk.newEphX25519Pub, b.ephPub, 32);
  espnowCryptoRandomBytes(rk.nonceRekey, 16);
  rk.prevTxSeqAtRekey = bTxSeq;
  uint8_t t[75];
  buildRekeyTranscript(t, rk.sessionId, rk.senderMac, rk.receiverMac, rk.newEphX25519Pub,
                       rk.nonceRekey, rk.prevTxSeqAtRekey);
  espnowCryptoEd25519Sign(rk.signature, t, sizeof t, b.idSec);
  return rk;
}

// The peer answers our REKEY as the production responder does.
static V4PayloadSessionRekey peerReply(PeerSide& b, const V4PayloadSessionRekey& fromUs) {
  V4PayloadSessionRekey rk = peerRekey(b, fromUs.sessionId, 7);
  peerDerive(b, fromUs.newEphX25519Pub);
  return rk;
}

// Deliver a peer REKEY the way v4hSessionRekey hands it to cmd_exec.
static void deliver(const PeerSide& b, const V4PayloadSessionRekey& rk) {
  auto* w = (DeferredRekeyWork*)ps_alloc(sizeof(DeferredRekeyWork), AllocPref::PreferPSRAM, "test");
  w->msg = rk;
  std::memcpy(w->srcMac, b.mac, 6);
  runDeferredRekey(w);  // frees w
}

static bool frameAsRekey(const SentFrame& f, V4PayloadSessionRekey* out) {
  if (f.type != ESPNOW_V4_TYPE_SESSION_REKEY || f.payload.size() != sizeof(V4PayloadSessionRekey)) return false;
  std::memcpy(out, f.payload.data(), sizeof *out);
  return true;
}

static SessionState* activeSession(PeerSide& b, uint16_t sessionId, uint32_t txSeq) {
  sessionsInit();
  const PeerIdentity* pid = peerIdentityFindByMac(b.mac);
  SessionState* s = sessionAllocate(b.mac, pid ? pid->meshId : 0);
  if (!s) return nullptr;
  s->myDirection = sessionIsASide(gSelfMac, b.mac) ? 0 : 1;
  b.myDirection = (uint8_t)(s->myDirection ^ 1);
  s->sessionId = sessionId;
  espnowCryptoRandomBytes(s->aeadKeyTx, 32);
  espnowCryptoRandomBytes(s->aeadKeyRx, 32);
  s->txSeqNext = txSeq;
  s->state = SESSION_ACTIVE;
  s->establishedAtMs = gNow - kRekeyAgeThresholdMs;  // keyed long ago: due for its periodic rekey
  s->lastUseMs = gNow;
  return s;
}

static void reset() {
  if (sessionsInit())
    for (uint8_t i = 0; i < sessionSlotCount(); i++) {
      const SessionState* c = sessionAt(i);
      if (c) sessionClear(const_cast<SessionState*>(c));
    }
  rekeyRetrySweep(gNow);  // releases entries whose session is gone; sends nothing
  gLog.clear();
  gSent.clear();
  gDrains = 0;
  gSendFails = false;
  gFailAllocTag = nullptr;
  gNow += 100000;
}

static bool keysConverged(const SessionState* s, const PeerSide& b) {
  return std::memcmp(s->aeadKeyTx, b.keyRx, 32) == 0 && std::memcmp(s->aeadKeyRx, b.keyTx, 32) == 0;
}

static unsigned logsContaining(const char* level, const char* needle) {
  unsigned n = 0;
  for (const auto& l : gLog)
    if (l.rfind(level, 0) == 0 && l.find(needle) != std::string::npos) n++;
  return n;
}

static unsigned gFailures = 0;
static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) gFailures++;
}

static void sweepAt(uint32_t offsetFrom, uint32_t base) {
  gNow = base + offsetFrom;
  rekeyRetrySweep(gNow);
}

int main() {
  makeIdentity(gSelf.pub, gSelf.sec);
  gSelf.valid = true;
  for (unsigned i = 0; i < kPeers; i++) {
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x01, (uint8_t)(0x10 + i)};
    std::memcpy(gB[i].mac, mac, 6);
    std::memcpy(gPeerTable[i].mac, mac, 6);
    gPeerTable[i].meshId = 0;
    makeIdentity(gPeerTable[i].longTermPub, gB[i].idSec);
    gPeerTable[i].valid = true;
  }
  PeerSide& B = gB[0];
  V4PayloadSessionRekey rk = {};

  // c0: the in-flight table cannot be allocated. The REKEY still goes out and
  // the failure is logged once; there is no retry coverage (runs first, while
  // the table is still unallocated).
  reset();
  SessionState* s = activeSession(B, 0x1111, 42);
  gFailAllocTag = "espnow.rekey.inflight";
  check(s && espnowRekeyInitiate(B.mac) && gSent.size() == 1 && s->state == SESSION_REKEYING,
        "c0 table alloc failure: the REKEY is still sent");
  check(logsContaining("ERROR", "in-flight table") == 1,
        "c0 table alloc failure is logged once (FAILS ON BASE: no table)");
  sweepAt(2000, gNow);
  check(gSent.size() == 1, "c0 no retransmit without a table");

  // c1: no loss. One REKEY, the reply converges the keys, nothing is re-sent.
  reset();
  s = activeSession(B, 0x1234, 42);
  check(espnowRekeyInitiate(B.mac), "c1 initiate accepted");
  check(gSent.size() == 1 && s->state == SESSION_REKEYING && gSent[0].ttl == 1 &&
        std::memcmp(gSent[0].dst, B.mac, 6) == 0 &&
        gSent[0].flags == (ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ),
        "c1 one REKEY (HANDSHAKE|ACK_REQ, ttl 1) sent, session REKEYING");
  if (frameAsRekey(gSent[0], &rk)) deliver(B, peerReply(B, rk));
  check(s->state == SESSION_ACTIVE && keysConverged(s, B) && gDrains == 1,
        "c1 reply applied: ACTIVE, keys converged, pending ring drained");
  sweepAt(10000, gNow);
  check(gSent.size() == 1 && s->state == SESSION_ACTIVE, "c1 no retransmit after the reply");

  // c2: the first REKEY is lost. The retransmit is the identical frame with
  // the original msgId, and the peer's answer to it converges the keys.
  reset();
  s = activeSession(B, 0x2345, 42);
  espnowRekeyInitiate(B.mac);
  uint32_t t0 = gNow;
  sweepAt(1999, t0);
  check(gSent.size() == 1, "c2 no retransmit before 2 s");
  sweepAt(2000, t0);
  check(gSent.size() == 2, "c2 exactly one retransmit at 2 s (FAILS ON BASE)");
  if (gSent.size() == 2) {
    check(gSent[1].msgId == gSent[0].msgId && gSent[1].payload == gSent[0].payload &&
          gSent[1].type == gSent[0].type && gSent[1].flags == gSent[0].flags &&
          gSent[1].ttl == gSent[0].ttl && std::memcmp(gSent[1].dst, B.mac, 6) == 0,
          "c2 retransmit carries the identical bytes, msgId, flags and ttl");
    if (frameAsRekey(gSent[1], &rk)) deliver(B, peerReply(B, rk));
    check(s->state == SESSION_ACTIVE && keysConverged(s, B),
          "c2 answer to the retransmit converges the keys");
    sweepAt(10000, t0);
    check(gSent.size() == 2, "c2 nothing more after the answer");
  }

  // c3: every copy is lost. Two retransmits, then the session is dropped so
  // the next send re-handshakes.
  reset();
  s = activeSession(B, 0x3456, 42);
  espnowRekeyInitiate(B.mac);
  t0 = gNow;
  sweepAt(2000, t0);
  sweepAt(4000, t0);
  check(gSent.size() == 3, "c3 two retransmits at 2 s and 4 s (FAILS ON BASE)");
  check(s->state == SESSION_REKEYING, "c3 still REKEYING until the last interval ends");
  const size_t sentBy4s = gSent.size();
  sweepAt(5999, t0);
  check(s->state == SESSION_REKEYING && gSent.size() == sentBy4s, "c3 no give-up and no send before 6 s");
  sweepAt(6000, t0);
  check(s->state == SESSION_FREE && sessionFindByPeer(B.mac, 0) == nullptr,
        "c3 session dropped at 6 s (FAILS ON BASE: stays REKEYING)");
  check(logsContaining("WARN", "dropping the session") == 1, "c3 one give-up WARN (FAILS ON BASE)");
  check(sodium_is_zero(s->rekeyEphPrivKey, 32) == 1, "c3 parked eph priv wiped (FAILS ON BASE)");
  const size_t sentBy6s = gSent.size();
  sweepAt(8000, t0);
  check(gSent.size() == sentBy6s, "c3 nothing is sent after the give-up");
  // Preserved: the ESTABLISHING sweep never touches a REKEYING session.
  reset();
  s = activeSession(B, 0x3457, 1);
  check(espnowRekeyInitiate(B.mac), "c3b initiate");
  gNow += 100000;
  check(sessionEstablishingTimeoutSweep(gNow) == 0 && s->state == SESSION_REKEYING,
        "c3b ESTABLISHING sweep leaves REKEYING alone (preserved)");

  // c4: responder path. A REKEY from the peer on an ACTIVE session gets one
  // reply, keys converge, and the reply is never re-sent.
  reset();
  s = activeSession(B, 0x4567, 42);
  deliver(B, peerRekey(B, 0x4567, 3));
  check(gSent.size() == 1 && s->state == SESSION_ACTIVE, "c4 responder replied once and is ACTIVE");
  if (!gSent.empty() && frameAsRekey(gSent[0], &rk)) peerDerive(B, rk.newEphX25519Pub);
  check(keysConverged(s, B), "c4 responder keys converge");
  sweepAt(10000, gNow);
  check(gSent.size() == 1 && s->state == SESSION_ACTIVE, "c4 a responder reply is never re-sent");

  // c5: an entry is released by session state. (a) aborted rekey;
  // (b) session cleared and replaced by a fresh handshake.
  reset();
  s = activeSession(B, 0x5678, 42);
  espnowRekeyInitiate(B.mac);
  sessionAbortRekey(s);
  sweepAt(2000, gNow);
  check(gSent.size() == 1 && s->state == SESSION_ACTIVE, "c5a aborted rekey: no retransmit, stays ACTIVE");
  reset();
  s = activeSession(B, 0x6789, 42);
  espnowRekeyInitiate(B.mac);
  sessionClear(s);
  s = activeSession(B, 0x789A, 1);
  t0 = gNow;
  for (uint32_t step = 1; step <= 4; step++) sweepAt(2000 * step, t0);
  check(gSent.size() == 1 && s->state == SESSION_ACTIVE && s->sessionId == 0x789A,
        "c5b replacement session is neither re-sent to nor cleared");

  // c6: the radio refuses the REKEY. Nothing reached the peer, so the session
  // stays usable on its current keys, and the re-init guard still spaces the
  // next attempt by kRekeyMinIntervalMs.
  reset();
  s = activeSession(B, 0x89AB, 42);
  gSendFails = true;
  check(!espnowRekeyInitiate(B.mac), "c6 initiate reports the refused send");
  check(s->state == SESSION_ACTIVE && sodium_is_zero(s->rekeyEphPrivKey, 32) == 1,
        "c6 refused send leaves the session ACTIVE (FAILS ON BASE: REKEYING)");
  gSendFails = false;
  t0 = gNow;
  sweepAt(2000, t0);
  check(gSent.empty(), "c6 nothing armed after a refused send");
  gNow = t0 + 10;
  check(!espnowRekeyInitiate(B.mac) && gSent.empty(), "c6 immediate re-initiate is held off");
  gNow = t0 + kRekeyMinIntervalMs;
  check(espnowRekeyInitiate(B.mac) && gSent.size() == 1 && s->state == SESSION_REKEYING,
        "c6 re-initiate after kRekeyMinIntervalMs sends (FAILS ON BASE)");

  // c7: every session reaches the age trigger in the same tick. All sixteen
  // outstanding REKEYs are covered, and all sixteen sessions are dropped when
  // nobody answers.
  reset();
  SessionState* all[kPeers] = {};
  for (unsigned i = 0; i < kPeers; i++) all[i] = activeSession(gB[i], (uint16_t)(0x7000 + i), 5);
  unsigned initiated = 0;
  for (unsigned i = 0; i < kPeers; i++) initiated += espnowRekeyInitiate(gB[i].mac) ? 1u : 0u;
  check(initiated == kPeers && gSent.size() == kPeers, "c7 sixteen REKEYs sent");
  t0 = gNow;
  sweepAt(2000, t0);
  check(gSent.size() == 2 * kPeers, "c7 every outstanding REKEY is re-sent (FAILS ON BASE)");
  sweepAt(4000, t0);
  sweepAt(6000, t0);
  unsigned freed = 0;
  for (unsigned i = 0; i < kPeers; i++) freed += (all[i] && all[i]->state == SESSION_FREE) ? 1u : 0u;
  check(freed == kPeers && gSent.size() == 3 * kPeers,
        "c7 every unanswered session is dropped (FAILS ON BASE)");

  // c8: the espnow tick runs the auto-trigger every 10 ms. (a) The radio
  // refuses the triggered REKEY, so the session is ACTIVE and still due: no
  // line per tick until kRekeyMinIntervalMs after the attempt, then exactly one
  // more attempt. (b) A session keyed just now that is already over the tx
  // threshold: no line per tick before its first attempt at kRekeyMinIntervalMs.
  constexpr uint32_t kTickMs = 10;
  reset();
  s = activeSession(B, 0x9ABC, 42);
  gSendFails = true;
  t0 = gNow;
  for (gNow = t0; gNow < t0 + kRekeyMinIntervalMs; gNow += kTickMs) autoRekeyTick();
  check(logsContaining("INFO", "REKEY auto-trigger") == 1 && logsContaining("DEBUG", "skipping") == 0,
        "c8a refused auto-trigger REKEY: one INFO line and no per-tick 'skipping' line within "
        "kRekeyMinIntervalMs (FAILS without the tick guard: one of each per 10 ms tick)");
  gSendFails = false;
  const unsigned infoBefore = logsContaining("INFO", "REKEY auto-trigger");
  autoRekeyTick();
  check(logsContaining("INFO", "REKEY auto-trigger") == infoBefore + 1 && gSent.size() == 1 &&
        s->state == SESSION_REKEYING,
        "c8a the tick attempts again once kRekeyMinIntervalMs has passed (FAILS ON BASE: refused send left REKEYING)");
  reset();
  s = activeSession(B, 0x9ABD, kRekeyTxFramesThreshold);
  s->establishedAtMs = gNow;
  t0 = gNow;
  for (gNow = t0; gNow < t0 + kRekeyMinIntervalMs; gNow += kTickMs) autoRekeyTick();
  check(logsContaining("INFO", "REKEY auto-trigger") + logsContaining("DEBUG", "skipping") <= 1,
        "c8b fresh session over the tx threshold: no per-tick lines within kRekeyMinIntervalMs of the "
        "key change (FAILS without the tick guard)");
  autoRekeyTick();
  check(gSent.size() == 1 && s->state == SESSION_REKEYING,
        "c8b one REKEY has gone out by kRekeyMinIntervalMs after the key change");

  // Lock discipline across all cases: no frame is sent and no pending drain
  // runs while EspNowTxGuard is held.
  check(gSendsUnderGuard == 0 && gDrainsUnderGuard == 0 && gGuardDepth == 0,
        "no send or drain runs under EspNowTxGuard");

  std::printf("%u failure(s)\n", gFailures);
  return gFailures ? 1 : 0;
}
