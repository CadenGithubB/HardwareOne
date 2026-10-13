// Host harness: production SESSION_OPEN / SESSION_CONFIRM / SESSION_REKEY
// handlers and initiators from System_ESPNow_Handlers_Crypto.cpp, with the
// session lifecycle, wrap and unwrap functions from System_ESPNow_Sessions.cpp,
// running on two modelled devices that exchange real handshake frames.
// Doubles: a settable clock, per-device logs, outboxes and deferred (cmd_exec)
// queues, calloc-backed ps_alloc, a counting lock and deterministic stand-in
// crypto (a commutative "X25519", an XOR KDF, a keyed "Ed25519" over the whole
// transcript and a keyed AEAD); libsodium is a library call, not logic under
// test. No radio, no hardware.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

// ---- Deadline owner (System_ESPNow_Deadline.h, when the tree has it) ---------
// INSERT_DEADLINE

// ---- Production wire types: flags, header, handshake payloads ---------------
// INSERT_WIRE

// ---- Production session record, constants, identities, RX context -----------
struct esp_now_recv_info {
  const uint8_t* src_addr;
  const uint8_t* des_addr;
};
// INSERT_TYPES
static_assert(kRekeyMinIntervalMs > kRekeyPrevKeysWindowMs && kRekeyAgeThresholdMs > kRekeyMinIntervalMs,
              "header constants referenced");

// ---- Modelled devices ----------------------------------------------------------
using DeferredFn = void (*)(void*);
struct Frame {
  uint8_t src[6];
  uint8_t dst[6];
  uint8_t type;
  uint16_t flags;
  uint32_t msgId;
  std::vector<uint8_t> payload;
};
constexpr unsigned kTableSlots = 16;
struct Device {
  const char* name;
  uint8_t mac[6];
  EspNowIdentity id;
  PeerIdentity peer;                 // the other device's stored identity
  SessionState table[kTableSlots];   // this device's gSessions
  std::deque<std::pair<DeferredFn, void*>> queue;  // cmd_exec_task work
  std::vector<Frame> outbox;         // every frame this device sent
  size_t delivered;                  // outbox frames already handed to the peer
  std::vector<std::string> log;
};
static Device A, B;
static Device* gCur = &A;  // the device whose code is running

// ---- Platform doubles -----------------------------------------------------------
static uint32_t gNow = 1000;
static uint32_t millis() { return gNow; }

__attribute__((format(printf, 2, 3)))
static void noteLog(const char* level, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  gCur->log.push_back(std::string(level) + ": " + buf);
}
#define ERROR_ESPNOWF(...) noteLog("ERROR", __VA_ARGS__)
#define WARN_ESPNOWF(...)  noteLog("WARN", __VA_ARGS__)
#define INFO_ESPNOWF(...)  noteLog("INFO", __VA_ARGS__)
#define DEBUG_ESPNOW_CORE 0
#define DEBUGF(flag, ...)  noteLog("DEBUG", __VA_ARGS__)

enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t n, AllocPref, const char*) { return std::calloc(1, n); }
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
static unsigned gDrains = 0, gBondNotes = 0;
static void pendingFrameDrainForPeer(const uint8_t*) { ++gDrains; }
static void bondNotifySessionEstablished(const uint8_t*) { ++gBondNotes; }

struct EspNowState { bool initialized; };
static EspNowState gStateObj{true};
static EspNowState* gEspNow = &gStateObj;

static const EspNowIdentity& espnowIdentityGet() { return gCur->id; }
static int espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, gCur->mac, 6); return 0; }
static const PeerIdentity* peerIdentityFindByMac(const uint8_t mac[6]) {
  const PeerIdentity& p = gCur->peer;
  return (p.valid && std::memcmp(mac, p.mac, 6) == 0) ? &p : nullptr;
}
static bool ensureUnencryptedPeer(const uint8_t*) { return true; }
static uint32_t gNextMsgId = 100;
static uint32_t generateMessageId() { return gNextMsgId++; }
static bool v4_send_frame(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                          const uint8_t* payload, uint16_t payloadLen, uint8_t) {
  Frame f;
  std::memcpy(f.src, gCur->mac, 6);
  std::memcpy(f.dst, dst, 6);
  f.type = type;
  f.flags = flags;
  f.msgId = msgId;
  f.payload.assign(payload, payload + payloadLen);
  gCur->outbox.push_back(f);
  return true;
}
static bool submitDeferredToCmdExec(DeferredFn fn, void* arg) {
  gCur->queue.emplace_back(fn, arg);
  return true;
}

// ---- Deterministic stand-in crypto ----------------------------------------------
static uint32_t gSeed = 7;
static void espnowCryptoRandomBytes(uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    gSeed = gSeed * 1103515245u + 12345u;
    out[i] = (uint8_t)(gSeed >> 16);
  }
}
// "X25519": pub = sec ^ 0x5A, shared(sec, pub) = (sec ^ 0x5A) ^ pub ^ 0x33.
// Commutative, so both ends of an exchange derive the same secret, and a wrong
// or wiped private key derives a different one.
static bool espnowCryptoX25519Keygen(uint8_t pub[32], uint8_t sec[32]) {
  espnowCryptoRandomBytes(sec, 32);
  for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(sec[i] ^ 0x5A);
  return true;
}
static bool espnowCryptoX25519Shared(uint8_t out[32], const uint8_t sec[32], const uint8_t pub[32]) {
  if (sodium_is_zero(pub, 32)) return false;
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)((sec[i] ^ 0x5A) ^ pub[i] ^ 0x33);
  return true;
}
static bool espnowCryptoKdfSubkey(uint8_t out[32], const uint8_t master[32], uint64_t id, const char* ctx) {
  size_t n = std::strlen(ctx);
  for (int i = 0; i < 32; i++) out[i] = (uint8_t)(master[i] ^ (uint8_t)(id * 0x3B) ^ (uint8_t)ctx[i % n]);
  return true;
}
static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
  return h;
}
// "Ed25519": sec = seed(32) || pub(32); the signature is a keyed digest of the
// whole message, recomputable from the public key, so any changed byte fails.
static void sigFor(uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t pub[32]) {
  uint64_t h = fnv(fnv(0xcbf29ce484222325ULL, pub, 32), m, n);
  for (int i = 0; i < 64; i++) {
    h ^= (uint64_t)i;
    h *= 0x100000001b3ULL;
    sig[i] = (uint8_t)(h >> 29);
  }
}
static bool espnowCryptoEd25519Sign(uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t sec[64]) {
  sigFor(sig, m, n, sec + 32);
  return true;
}
static bool espnowCryptoEd25519Verify(const uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t pub[32]) {
  uint8_t expect[64];
  sigFor(expect, m, n, pub);
  return std::memcmp(expect, sig, 64) == 0;
}
// Keyed AEAD: keystream and tag are digests over key || nonce (|| aad ||
// cipher); open succeeds only with the same key, nonce, AAD and ciphertext.
static uint64_t digest(uint64_t seed, const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t* aad, size_t aadLen, const uint8_t* body, size_t bodyLen) {
  uint64_t h = 0xcbf29ce484222325ULL ^ seed;
  h = fnv(h, key, 32);
  h = fnv(h, nonce, 12);
  h = fnv(h, aad, aadLen);
  return fnv(h, body, bodyLen);
}
static uint8_t keystreamByte(const uint8_t key[32], const uint8_t nonce[12], size_t i) {
  uint8_t idx[8];
  for (unsigned b = 0; b < 8; b++) idx[b] = (uint8_t)((i + 1) >> (8 * b));
  return (uint8_t)digest(0x5eed, key, nonce, idx, sizeof idx, nullptr, 0);
}
static void tagFor(uint8_t tag[16], const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* aad, size_t aadLen, const uint8_t* cipher, size_t cipherLen) {
  uint64_t a = digest(1, key, nonce, aad, aadLen, cipher, cipherLen);
  uint64_t b = digest(2, key, nonce, aad, aadLen, cipher, cipherLen);
  std::memcpy(tag, &a, 8);
  std::memcpy(tag + 8, &b, 8);
}
static bool espnowCryptoAeadSeal(uint8_t* cipherOut, uint8_t tagOut[16],
                                 const uint8_t* plaintext, size_t plaintextLen,
                                 const uint8_t* aad, size_t aadLen,
                                 const uint8_t nonce[12], const uint8_t key[32]) {
  for (size_t i = 0; i < plaintextLen; i++) cipherOut[i] = plaintext[i] ^ keystreamByte(key, nonce, i);
  tagFor(tagOut, key, nonce, aad, aadLen, cipherOut, plaintextLen);
  return true;
}
static bool espnowCryptoAeadOpen(uint8_t* plaintextOut,
                                 const uint8_t* cipher, size_t cipherLen, const uint8_t tag[16],
                                 const uint8_t* aad, size_t aadLen,
                                 const uint8_t nonce[12], const uint8_t key[32]) {
  uint8_t expect[16];
  tagFor(expect, key, nonce, aad, aadLen, cipher, cipherLen);
  if (std::memcmp(expect, tag, 16) != 0) return false;
  for (size_t i = 0; i < cipherLen; i++) plaintextOut[i] = cipher[i] ^ keystreamByte(key, nonce, i);
  return true;
}

// ---- Production: Sessions.cpp lifecycle, wrap/unwrap, rekey helpers ------------
// INSERT_SESSIONS

// ---- Production: Handlers_Crypto.cpp internals (anonymous namespace) -----------
namespace {
// INSERT_CRYPTO_INTERNAL
}  // namespace

// ---- Production: Handlers_Crypto.cpp entry points --------------------------------
// INSERT_CRYPTO_PUBLIC

static_assert(kTableSlots == kSessionSlots, "one modelled table per device");

// ---- Test driver ------------------------------------------------------------------
static void use(Device& d) {
  gCur = &d;
  gSessions = d.table;
}

static void runQueue(Device& d) {
  while (!d.queue.empty()) {
    auto job = d.queue.front();
    d.queue.pop_front();
    use(d);
    job.first(job.second);
  }
}

// Hand a frame to `to` the way the V4 dispatcher does (header + recv info),
// then let its cmd_exec queue run the deferred handshake work.
static void deliver(const Frame& f, Device& to) {
  use(to);
  EspNowV4Header h{};
  h.magic = 0x3148;
  h.ver = 4;
  h.type = f.type;
  h.flags = f.flags;
  h.headerLen = 32;
  h.msgId = f.msgId;
  std::memcpy(h.origin, f.src, 6);
  h.ttl = 1;
  h.fragCount = 1;
  esp_now_recv_info ri{f.src, to.mac};
  V4RxCtx ctx{};
  ctx.recv_info = &ri;
  ctx.h = &h;
  ctx.payload = f.payload.data();
  ctx.payloadLen = (uint16_t)f.payload.size();
  if (f.type == ESPNOW_V4_TYPE_SESSION_OPEN) v4hSessionOpen(ctx);
  else if (f.type == ESPNOW_V4_TYPE_SESSION_CONFIRM) v4hSessionConfirm(ctx);
  else if (f.type == ESPNOW_V4_TYPE_SESSION_REKEY) v4hSessionRekey(ctx);
  runQueue(to);
}

// The newest frame `d` sent; everything it sent so far counts as handled.
static Frame take(Device& d) {
  d.delivered = d.outbox.size();
  return d.outbox.empty() ? Frame{} : d.outbox.back();
}

// Deliver every not-yet-delivered frame to the other device until both are
// quiet or `maxDeliveries` frames moved. Returns the number delivered.
static unsigned exchange(unsigned maxDeliveries) {
  unsigned n = 0;
  while (n < maxDeliveries) {
    if (A.delivered < A.outbox.size()) {
      Frame f = A.outbox[A.delivered++];
      deliver(f, B);
    } else if (B.delivered < B.outbox.size()) {
      Frame f = B.outbox[B.delivered++];
      deliver(f, A);
    } else {
      break;
    }
    n++;
  }
  return n;
}

static SessionState* sessOf(Device& d) {
  use(d);
  return sessionFindByPeer(d.peer.mac, d.peer.meshId);
}

// Seal one frame on `from` with the real sessionWrapFrame and open it on `to`
// with the real sessionUnwrapFrame.
static bool sealOpen(Device& from, Device& to) {
  SessionState* s = sessOf(from);
  if (!s) return false;
  EspNowV4Header h{};
  h.magic = 0x3148;
  h.ver = 4;
  h.type = 50;
  h.headerLen = 32;
  h.msgId = generateMessageId();
  std::memcpy(h.origin, from.mac, 6);
  h.ttl = 1;
  h.fragCount = 1;
  const uint8_t plain[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t sealed[sizeof plain + 16];
  use(from);
  if (!sessionWrapFrame(s, &h, plain, sizeof plain, sealed)) return false;
  use(to);
  SessionState* r = sessionFindBySessionId(h.sessionId, from.mac);
  uint8_t out[sizeof sealed];
  uint16_t outLen = 0;
  return r && sessionUnwrapFrame(r, &h, sealed, sizeof sealed, out, &outLen) &&
         outLen == sizeof plain && std::memcmp(out, plain, sizeof plain) == 0;
}

// Both sides ACTIVE on the same session with mirrored keys, and a frame each
// way seals and opens.
static bool keysMirror() {
  SessionState* a = sessOf(A);
  SessionState* b = sessOf(B);
  if (!a || !b || a->state != SESSION_ACTIVE || b->state != SESSION_ACTIVE) return false;
  if (a->sessionId != b->sessionId) return false;
  if (std::memcmp(a->aeadKeyTx, b->aeadKeyRx, 32) != 0 || std::memcmp(a->aeadKeyRx, b->aeadKeyTx, 32) != 0)
    return false;
  return sealOpen(A, B) && sealOpen(B, A);
}

// A REKEY signed with `from`'s identity key for the live session (a frame
// `from` could have sent): fresh ephemeral, fresh nonce, given sequence.
static Frame signedRekey(Device& from, Device& to, uint32_t prevTxSeq) {
  SessionState* s = sessOf(from);
  V4PayloadSessionRekey rk{};
  rk.sessionId = s ? s->sessionId : 0;
  std::memcpy(rk.senderMac, from.mac, 6);
  std::memcpy(rk.receiverMac, to.mac, 6);
  uint8_t ephSec[32];
  espnowCryptoX25519Keygen(rk.newEphX25519Pub, ephSec);
  espnowCryptoRandomBytes(rk.nonceRekey, 16);
  rk.prevTxSeqAtRekey = prevTxSeq;
  uint8_t t[75];
  buildRekeyTranscript(t, rk.sessionId, rk.senderMac, rk.receiverMac, rk.newEphX25519Pub,
                       rk.nonceRekey, rk.prevTxSeqAtRekey);
  espnowCryptoEd25519Sign(rk.signature, t, sizeof t, from.id.sec);
  Frame f;
  std::memcpy(f.src, from.mac, 6);
  std::memcpy(f.dst, to.mac, 6);
  f.type = ESPNOW_V4_TYPE_SESSION_REKEY;
  f.flags = ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ;
  f.msgId = generateMessageId();
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&rk);
  f.payload.assign(p, p + sizeof rk);
  return f;
}

static unsigned logsContaining(const Device& d, const char* level, const char* needle) {
  unsigned n = 0;
  for (const auto& l : d.log)
    if (l.rfind(level, 0) == 0 && l.find(needle) != std::string::npos) n++;
  return n;
}

static unsigned gFailures = 0;
static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) gFailures++;
}

// Fresh session tables, queues, outboxes and logs on both devices; identities
// are kept. Time moves on so per-peer log throttles from earlier cases lapse.
static void resetDevices() {
  for (Device* d : {&A, &B}) {
    std::memset(d->table, 0, sizeof d->table);
    for (auto& job : d->queue) std::free(job.second);
    d->queue.clear();
    d->outbox.clear();
    d->delivered = 0;
    d->log.clear();
  }
  gNow += 100000;
}

// A opens, B confirms.
static bool establish() {
  use(A);
  if (!espnowSessionOpenInitiate(B.mac, "mesh") || A.outbox.empty()) return false;
  deliver(take(A), B);
  if (B.outbox.empty()) return false;
  deliver(take(B), A);
  return keysMirror();
}

// One complete rekey: `i` initiates, `r` replies, both apply. Returns the
// responder's reply through `reply`.
static bool rekeyRound(Device& i, Device& r, Frame* reply = nullptr) {
  const size_t i0 = i.outbox.size(), r0 = r.outbox.size();
  use(i);
  if (!espnowRekeyInitiate(r.mac) || i.outbox.size() != i0 + 1) return false;
  deliver(take(i), r);
  if (r.outbox.size() != r0 + 1) return false;
  Frame rf = take(r);
  if (reply) *reply = rf;
  deliver(rf, i);
  return i.outbox.size() == i0 + 1 && keysMirror();
}

static void makeDevice(Device& d, const char* name, uint8_t lastMacByte) {
  d.name = name;
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, lastMacByte};
  std::memcpy(d.mac, mac, 6);
  espnowCryptoRandomBytes(d.id.sec, 32);
  espnowCryptoRandomBytes(d.id.pub, 32);
  std::memcpy(d.id.sec + 32, d.id.pub, 32);
  d.id.valid = true;
}

static void pairIdentities() {
  for (auto pr : {std::make_pair(&A, &B), std::make_pair(&B, &A)}) {
    PeerIdentity& p = pr.first->peer;
    std::memset(&p, 0, sizeof p);
    std::memcpy(p.mac, pr.second->mac, 6);
    p.meshId = 0;
    std::memcpy(p.longTermPub, pr.second->id.pub, 32);
    p.valid = true;
  }
}

int main() {
  makeDevice(A, "A", 0x0A);  // numerically lower MAC: the A side
  makeDevice(B, "B", 0x0B);
  pairIdentities();
  char label[200];

  // C1 — establish (preserved).
  resetDevices();
  check(establish(), "C1 OPEN/CONFIRM establishes mirrored keys on both sides");
  check(A.outbox.size() == 1 && B.outbox.size() == 1 && sessOf(A)->sessionId == sessOf(B)->sessionId,
        "C1 one OPEN, one CONFIRM, one sessionId");

  // C2 — normal rekey (preserved): one initiation, one reply, both apply.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  use(A);
  check(espnowRekeyInitiate(B.mac) && A.outbox.size() == 2 && sessOf(A)->state == SESSION_REKEYING,
        "C2 A initiates: one REKEY sent, A REKEYING");
  check(A.outbox.back().type == ESPNOW_V4_TYPE_SESSION_REKEY &&
        A.outbox.back().flags == (ESPNOW_V4_FLAG_HANDSHAKE | ESPNOW_V4_FLAG_ACK_REQ),
        "C2 the REKEY is a HANDSHAKE|ACK_REQ frame");
  deliver(take(A), B);
  check(B.outbox.size() == 2 && sessOf(B)->state == SESSION_ACTIVE, "C2 B replies once and is ACTIVE");
  deliver(take(B), A);
  check(A.outbox.size() == 2 && keysMirror(), "C2 A applies the reply without sending; keys converge");
  check(logsContaining(A, "INFO", "SESSION rekeyed") == 1 && logsContaining(B, "INFO", "SESSION rekeyed") == 1,
        "C2 one 'SESSION rekeyed' per side");

  // C3 — the defect: B's reply re-delivered to A 40 s later.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  Frame rb1;
  check(rekeyRound(A, B, &rb1), "C3 setup: a completed rekey");
  gNow += 40000;
  size_t a0 = A.outbox.size(), b0 = B.outbox.size();
  deliver(rb1, A);
  check(A.outbox.size() == a0, "C3 a re-delivered REKEY is not answered (FAILS ON BASE: A replies)");
  check(keysMirror(), "C3 the live keys are unchanged (FAILS ON BASE: A installed keys B does not hold)");
  check(logsContaining(A, "WARN", "ignored (nonce already seen)") == 1,
        "C3 one WARN names the re-delivery (FAILS ON BASE)");
  exchange(12);
  size_t extra = (A.outbox.size() - a0) + (B.outbox.size() - b0);
  std::snprintf(label, sizeof label,
                "C3 no exchange follows the copy (FAILS ON BASE: endless REKEY ping-pong) [%zu frames]", extra);
  check(extra == 0, label);

  // C4 — an older copy (not the last nonce, past the interval, inside the
  // window) is answered once; the peer's answer ends the exchange.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  rekeyRound(A, B, &rb1);
  gNow += kRekeyAgeThresholdMs;
  check(rekeyRound(A, B), "C4 setup: two completed rekeys");
  gNow += 40000;
  a0 = A.outbox.size();
  b0 = B.outbox.size();
  deliver(rb1, A);
  check(A.outbox.size() == a0 + 1, "C4 the older copy is answered once (preserved)");
  exchange(12);
  std::snprintf(label, sizeof label,
                "C4 one extra round, then the exchange ends (FAILS ON BASE: ping-pong) [A %zu, B %zu frames]",
                A.outbox.size() - a0, B.outbox.size() - b0);
  check(A.outbox.size() == a0 + 1 && B.outbox.size() == b0 + 1, label);
  check(keysMirror(), "C4 both sides converge on the same keys (FAILS ON BASE)");
  check(logsContaining(A, "INFO", "peer answered our reply") == 1,
        "C4 A applies B's answer to its reply as the counterpart (FAILS ON BASE)");
  gNow += kRekeyPrevKeysWindowMs + 1;
  use(A);
  sessionRekeyPrevKeysSweep(gNow);
  use(B);
  sessionRekeyPrevKeysSweep(gNow);
  a0 = A.outbox.size();
  deliver(B.outbox.back(), A);
  check(A.outbox.size() == a0 && keysMirror(),
        "C4 a further copy of B's last REKEY is ignored (FAILS ON BASE)");

  // C5 — applied-at minimum interval, receive side and initiator side.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  rekeyRound(A, B);
  gNow += 10000;
  a0 = A.outbox.size();
  deliver(signedRekey(B, A, sessOf(B)->txSeqNext), A);
  check(A.outbox.size() == a0, "C5 a fresh initiation 10 s after the keys changed is not answered (FAILS ON BASE)");
  check(keysMirror(), "C5 keys unchanged (FAILS ON BASE)");
  check(logsContaining(A, "WARN", "session keyed <30s ago") == 1, "C5 one WARN names the interval (FAILS ON BASE)");
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  rekeyRound(A, B);
  gNow += 10000;
  a0 = A.outbox.size();
  b0 = B.outbox.size();
  use(A);
  bool sentA = espnowRekeyInitiate(B.mac);
  use(B);
  bool sentB = espnowRekeyInitiate(A.mac);
  check(!sentA && !sentB && A.outbox.size() == a0 && B.outbox.size() == b0 && keysMirror(),
        "C5 neither side initiates within kRekeyMinIntervalMs of the last apply (FAILS ON BASE)");
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  rekeyRound(A, B);
  gNow += kRekeyMinIntervalMs;
  check(rekeyRound(A, B), "C5 a rekey kRekeyMinIntervalMs after the last one completes (preserved)");

  // C6 — sequence window: an initiation signed 100 frames ago is stale.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  use(B);
  espnowRekeyInitiate(A.mac);
  Frame rb0 = take(B);
  sessionAbortRekey(sessOf(B));
  bool all = true;
  for (int i = 0; i < 100; i++) all = sealOpen(B, A) && all;
  check(all && sessOf(A)->rxSeqHighWater > 100, "C6 setup: 100 frames from B after its unsent REKEY");
  gNow += 40000;
  a0 = A.outbox.size();
  deliver(rb0, A);
  check(A.outbox.size() == a0, "C6 a REKEY signed 100 frames behind is not answered (FAILS ON BASE)");
  check(logsContaining(A, "WARN", "stale sequence") == 1, "C6 one WARN names the stale sequence (FAILS ON BASE)");
  check(keysMirror(), "C6 keys unchanged (FAILS ON BASE)");
  use(B);
  check(espnowRekeyInitiate(A.mac), "C6 control: B initiates at its live sequence");
  deliver(take(B), A);
  deliver(take(A), B);
  check(keysMirror(), "C6 control: answered and converged (preserved)");

  // C7 — the counterpart path takes no sequence check: a reply signed in the
  // peer's new epoch (low txSeqNext) still completes our initiation.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  for (int i = 0; i < 100; i++) sealOpen(B, A);
  a0 = A.outbox.size();
  use(A);
  espnowRekeyInitiate(B.mac);
  sessOf(B)->txSeqNext = 2;  // as just after an apply on B
  deliver(take(A), B);
  deliver(take(B), A);
  check(A.outbox.size() == a0 + 1 && keysMirror(),
        "C7 a reply far below our rx high-water mark still completes our REKEY (preserved)");

  // C8 — concurrent initiation (preserved).
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  use(A);
  espnowRekeyInitiate(B.mac);
  use(B);
  espnowRekeyInitiate(A.mac);
  Frame ia = take(A), ib = take(B);
  deliver(ia, B);
  deliver(ib, A);
  check(A.outbox.size() == 2 && B.outbox.size() == 2 && keysMirror(),
        "C8 both initiate at once: one REKEY each, both converge");

  // C9 — the same initiation delivered twice (a retransmit after B applied).
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  use(A);
  espnowRekeyInitiate(B.mac);
  ia = take(A);
  b0 = B.outbox.size();
  deliver(ia, B);
  Frame rb = take(B);
  gNow += 2000;
  deliver(ia, B);
  check(B.outbox.size() == b0 + 1, "C9 a second copy of the same REKEY gets no second reply (FAILS ON BASE)");
  check(logsContaining(B, "WARN", "ignored (nonce already seen)") == 1, "C9 one WARN on B (FAILS ON BASE)");
  deliver(rb, A);
  check(keysMirror(), "C9 A applies B's one reply and the keys converge (FAILS ON BASE)");

  // C10 — the defect: A's SESSION_OPEN re-delivered to B 10 s later.
  resetDevices();
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  Frame o1 = take(A);
  deliver(o1, B);
  deliver(take(B), A);
  check(keysMirror(), "C10 setup: session established");
  const uint16_t s1 = sessOf(B)->sessionId;
  gNow += 10000;
  b0 = B.outbox.size();
  deliver(o1, B);
  check(B.outbox.size() == b0, "C10 a re-delivered SESSION_OPEN gets no CONFIRM (FAILS ON BASE)");
  check(sessOf(B)->sessionId == s1 && keysMirror(),
        "C10 the live session and its keys are kept (FAILS ON BASE: B reset to keys A cannot derive)");
  check(logsContaining(B, "WARN", "re-delivered OPEN") == 1, "C10 one WARN names the re-delivery (FAILS ON BASE)");

  // C11 — A reboots and re-handshakes with a new id (preserved); a copy of
  // the OPEN of the session B's slot held before is then ignored.
  resetDevices();
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  o1 = take(A);
  deliver(o1, B);
  deliver(take(B), A);
  const uint16_t first = sessOf(B)->sessionId;
  std::memset(A.table, 0, sizeof A.table);  // A reboots: empty session table, same identity
  gNow += 20000;
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  deliver(take(A), B);
  deliver(take(B), A);
  const uint16_t second = sessOf(B)->sessionId;
  check(keysMirror() && second != first, "C11 a rebooted peer re-handshakes with a new sessionId (preserved)");
  gNow += 10000;
  b0 = B.outbox.size();
  deliver(o1, B);
  check(B.outbox.size() == b0 && sessOf(B)->sessionId == second && keysMirror(),
        "C11 a copy of the previous session's OPEN is ignored (FAILS ON BASE)");

  // C12 — OPEN glare (preserved): the A side keeps its initiation.
  resetDevices();
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  use(B);
  espnowSessionOpenInitiate(A.mac, "mesh");
  Frame oa = take(A), ob = take(B);
  deliver(ob, A);
  check(A.outbox.size() == 1 && logsContaining(A, "WARN", "glare") == 1, "C12 A (A side) ignores B's OPEN");
  deliver(oa, B);
  deliver(take(B), A);
  V4PayloadSessionOpen oaMsg;
  std::memcpy(&oaMsg, oa.payload.data(), sizeof oaMsg);
  check(keysMirror() && sessOf(A)->sessionId == oaMsg.sessionId,
        "C12 B yields; both converge on A's session");

  // C13 — the responder's reply grace ends after kRekeyPrevKeysWindowMs; a
  // later initiation inside the interval is refused, not applied.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  rekeyRound(A, B);
  const uint32_t appliedAt = gNow;
  check(!sodium_is_zero(sessOf(B)->rekeyEphPrivKey, 32),
        "C13 the responder keeps its reply key for the grace (FAILS ON BASE: no grace)");
  gNow = appliedAt + kRekeyPrevKeysWindowMs + 1;
  use(B);
  sessionRekeyPrevKeysSweep(gNow);
  check(sodium_is_zero(sessOf(B)->rekeyEphPrivKey, 32) == 1, "C13 the sweep wipes it when the grace ends");
  gNow = appliedAt + 6000;
  b0 = B.outbox.size();
  deliver(signedRekey(A, B, sessOf(A)->txSeqNext), B);
  check(B.outbox.size() == b0 && keysMirror() && logsContaining(B, "WARN", "session keyed <30s ago") == 1,
        "C13 a fresh REKEY 6 s after the apply is refused for the interval (FAILS ON BASE)");

  // C14 — a new handshake on a slot that is mid-REKEY drops the parked
  // rekey state (the partial reset), then completes normally.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  use(A);
  espnowRekeyInitiate(B.mac);
  take(A);
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  SessionState* sa = sessOf(A);
  check(sa->state == SESSION_ESTABLISHING && sodium_is_zero(sa->rekeyEphPrivKey, 32) == 1 &&
        sa->rekeyInitiatedAtMs == 0,
        "C14 re-opening a REKEYING slot drops the parked rekey key (FAILS ON BASE)");
  deliver(take(A), B);
  deliver(take(B), A);
  check(keysMirror(), "C14 the new handshake completes (preserved)");

  check(gGuardDepth == 0, "no lock left held");
  std::printf("%u failure(s)\n", gFailures);
  return gFailures ? 1 : 0;
}
