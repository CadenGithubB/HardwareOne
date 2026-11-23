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
#include "System_MacText.h"  // production MAC text helpers (copied beside this unit)
#include "System_ESPNow_HandshakeCore.h"  // production handshake byte layouts (copied beside this unit)

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

// ---- Mesh keys (1.0 step 4): each device holds its own copy of mesh slot 0's
// derived keys; the C1 mesh tags and the C2 session keys use the bootstrap key.
struct MeshDerivedKeys { bool valid; uint16_t fingerprint; uint8_t bootstrapKey[32]; uint8_t groupKey[32]; };
struct Settings {
  static constexpr uint8_t N_MESHES = 4;
  struct Mesh { uint16_t fingerprint; std::string label; };
  Mesh meshes[N_MESHES];
};
static Settings gSettings = {{{0x1234, "mesh"}, {0, ""}, {0, ""}, {0, ""}}};

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
  std::deque<std::pair<DeferredFn, void*>> queue;  // cmd_exec work
  std::vector<Frame> outbox;         // every frame this device sent
  size_t delivered;                  // outbox frames already handed to the peer
  std::vector<std::string> log;
  uint32_t caps;                     // this device's session capability word
  MeshDerivedKeys mesh;              // this device's keys of mesh slot 0
  uint16_t meshGen;                  // their generation (steps on a passphrase change)
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
#define DEBUG_ESPNOWF(...) noteLog("DEBUG", __VA_ARGS__)

enum class AllocPref { PreferPsram };
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
static const MeshDerivedKeys* meshKeysGet(uint8_t slot) {
  return slot == 0 && gCur->mesh.valid ? &gCur->mesh : nullptr;
}
static uint16_t meshKeysGeneration(uint8_t slot) { return slot == 0 ? gCur->meshGen : 0; }
// The clock a SESSION_OPEN signs (openEpochMs); 0 = not set.
namespace Clock {
static int64_t gEpochMs = 1800000000123LL;
static int64_t epochMillisOrZero() { return gEpochMs; }
}  // namespace Clock
// The image's capability word (production: System_ESPNow_Handlers_Crypto.cpp),
// per modelled device; a case changes it to model a reflash.
[[maybe_unused]] static uint32_t espnowLocalSessionCaps() { return gCur->caps; }
static int espnowSelfStaMac(uint8_t out[6]) { std::memcpy(out, gCur->mac, 6); return 0; }
static const PeerIdentity* peerIdentityFindByMac(const uint8_t mac[6]) {
  const PeerIdentity& p = gCur->peer;
  return (p.valid && std::memcmp(mac, p.mac, 6) == 0) ? &p : nullptr;
}
static bool ensureUnencryptedPeer(const uint8_t*) { return true; }
// The revoked list (System_ESPNow_Revoked.h): a case revokes the sender.
static bool gSenderRevoked = false;
[[maybe_unused]] static bool espnowRevokedMacOrKey(const uint8_t*, const uint8_t*) { return gSenderRevoked; }
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
[[maybe_unused]] static bool espnowCryptoX25519Public(uint8_t pub[32], const uint8_t sec[32]) {
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
// A keyed digest standing in for HMAC-SHA256.
static bool espnowCryptoHmacSha256(uint8_t out[32], const uint8_t* key, size_t keyLen, const uint8_t* in1,
                                   size_t len1, const uint8_t* in2, size_t len2, const uint8_t* in3, size_t len3) {
  uint64_t h = fnv(0x84222325cbf29ce4ULL, key, keyLen);
  h = fnv(fnv(fnv(h, in1, len1), in2, len2), in3, len3);
  for (int i = 0; i < 32; i++) { h ^= (uint64_t)i; h *= 0x100000001b3ULL; out[i] = (uint8_t)(h >> 29); }
  return true;
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
  use(from);
  sessionSealRekey(rk, 0, from.id.sec);   // signed, and tagged under `from`'s mesh key
  Frame f;
  std::memcpy(f.src, from.mac, 6);
  std::memcpy(f.dst, to.mac, 6);
  f.type = ESPNOW_V4_TYPE_SESSION_REKEY;
  f.flags = ESPNOW_V4_FLAG_ACK_REQ;
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

// A SESSION_OPEN kick from `from` to `to`, with the "already in flight" report
// where the source has one (HW1_OPEN_REPORTS_IN_FLIGHT, set by the driver).
static bool openKick(Device& from, Device& to, bool* inFlight) {
  use(from);
#ifdef HW1_OPEN_REPORTS_IN_FLIGHT
  return espnowSessionOpenInitiate(to.mac, "mesh", inFlight);
#else
  *inFlight = false;
  return espnowSessionOpenInitiate(to.mac, "mesh");
#endif
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
  d.caps = 0;
  d.mesh.valid = true;
  d.mesh.fingerprint = 0x1234;
  std::memset(d.mesh.bootstrapKey, 0x61, 32);   // both devices hold the same mesh passphrase
  std::memset(d.mesh.groupKey, 0x62, 32);
  d.meshGen = 1;
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
  A.caps = 0x000000FEu;      // every 1.0 bit but bond
  B.caps = 0x000100FFu;      // every 1.0 bit, and a reserved one from "later firmware"
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
        A.outbox.back().flags == ESPNOW_V4_FLAG_ACK_REQ,
        "C2 the REKEY is a plaintext ACK_REQ frame (no HANDSHAKE flag at 1.0)");
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

  // C12 — OPEN glare (preserved): the A side keeps its initiation, and sends
  // it once more, unchanged (2026-11-18): B's OPEN shows B hears now.
  resetDevices();
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  use(B);
  espnowSessionOpenInitiate(A.mac, "mesh");
  Frame oa = take(A), ob = take(B);
  deliver(ob, A);
  check(logsContaining(A, "WARN", "glare") == 1 && sessOf(A)->state == SESSION_ESTABLISHING,
        "C12 A (A side) ignores B's OPEN");
  check(A.outbox.size() == 2 && A.outbox[1].type == ESPNOW_V4_TYPE_SESSION_OPEN &&
        A.outbox[1].payload == oa.payload && A.outbox[1].msgId != oa.msgId,
        "C12 and sends its own OPEN once more, byte for byte (FAILS ON BASE: no copy)");
  const Frame oaCopy = A.outbox[1];
  A.delivered = A.outbox.size();
  deliver(oa, B);
  deliver(take(B), A);
  V4PayloadSessionOpen oaMsg;
  std::memcpy(&oaMsg, oa.payload.data(), sizeof oaMsg);
  check(keysMirror() && sessOf(A)->sessionId == oaMsg.sessionId,
        "C12 B yields; both converge on A's session");
  b0 = B.outbox.size();
  deliver(oaCopy, B);
  check(B.outbox.size() == b0 && keysMirror(), "C12 the copy reaching B after it answered is ignored");

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
  // rekey state (the partial reset), then completes normally. The REKEY is
  // long past its retry budget here: one still in flight is left alone (C16).
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  use(A);
  espnowRekeyInitiate(B.mac);
  take(A);
  gNow += 2 * kSessionEstablishTimeoutMs;
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  SessionState* sa = sessOf(A);
  check(sa->state == SESSION_ESTABLISHING && sodium_is_zero(sa->rekeyEphPrivKey, 32) == 1 &&
        sa->rekeyInitiatedAtMs == 0,
        "C14 re-opening a REKEYING slot drops the parked rekey key (FAILS ON BASE)");
  deliver(take(A), B);
  deliver(take(B), A);
  check(keysMirror(), "C14 the new handshake completes (preserved)");

  // ---- SESSION_OPEN kicks while a handshake or rekey is in flight -----------
  // Senders kick espnowSessionOpenInitiate whenever they find the session not
  // ACTIVE. "FAILS ON BASE" below: the tree where every kick re-allocated the
  // slot (keys zeroed, new sessionId, a second SESSION_OPEN).

  // C15 — kicks while our SESSION_OPEN awaits its CONFIRM.
  resetDevices();
  bool inFlight = true;
  check(openKick(A, B, &inFlight) && !inFlight && A.outbox.size() == 1 &&
        sessOf(A) && sessOf(A)->state == SESSION_ESTABLISHING,
        "C15 a kick with no session sends one SESSION_OPEN (preserved)");
  o1 = take(A);
  SessionState before;
  std::memcpy(&before, sessOf(A), sizeof before);
  bool allReported = true, allTrue = true;
  for (int i = 0; i < 3; i++) {
    gNow += 1500;
    inFlight = false;
    allTrue = openKick(A, B, &inFlight) && allTrue;
    allReported = allReported && inFlight;
  }
  check(allTrue && allReported, "C15 three kicks during ESTABLISHING report a handshake in flight (FAILS ON BASE)");
  check(A.outbox.size() == 1, "C15 they send no second SESSION_OPEN (FAILS ON BASE)");
  check(std::memcmp(&before, sessOf(A), sizeof before) == 0,
        "C15 the slot, its stashed handshake secrets and its sessionId are untouched (FAILS ON BASE)");
  deliver(o1, B);
  deliver(take(B), A);
  check(keysMirror(), "C15 the CONFIRM of the first OPEN still completes the session (FAILS ON BASE)");

  // C16 — a kick while a REKEY awaits the peer's answer.
  resetDevices();
  establish();
  gNow += kRekeyAgeThresholdMs;
  use(A);
  espnowRekeyInitiate(B.mac);
  Frame rk = take(A);
  std::memcpy(&before, sessOf(A), sizeof before);
  a0 = A.outbox.size();
  gNow += 1000;
  inFlight = false;
  check(openKick(A, B, &inFlight) && inFlight, "C16 a kick during REKEYING reports a handshake in flight (FAILS ON BASE)");
  check(A.outbox.size() == a0 && sessOf(A)->state == SESSION_REKEYING &&
        std::memcmp(&before, sessOf(A), sizeof before) == 0,
        "C16 the working keys, the parked rekey key and the state are untouched (FAILS ON BASE)");
  deliver(rk, B);
  deliver(take(B), A);
  check(keysMirror() && std::memcmp(sessOf(A)->aeadKeyTx, before.aeadKeyTx, 32) != 0,
        "C16 the rekey then completes on new keys (FAILS ON BASE)");

  // C17 — a kick on a CLOSED slot, and one on an ACTIVE session (the explicit
  // re-handshake), each start a new handshake (preserved).
  resetDevices();
  establish();
  uint16_t idBefore = sessOf(A)->sessionId;
  sessOf(A)->state = SESSION_CLOSED;
  a0 = A.outbox.size();
  inFlight = true;
  check(openKick(A, B, &inFlight) && !inFlight && A.outbox.size() == a0 + 1 &&
        sessOf(A)->state == SESSION_ESTABLISHING && sessOf(A)->sessionId != idBefore,
        "C17 a kick on a CLOSED slot sends a SESSION_OPEN with a new sessionId");
  deliver(take(A), B);
  deliver(take(B), A);
  check(keysMirror(), "C17 and the handshake completes");
  idBefore = sessOf(A)->sessionId;
  a0 = A.outbox.size();
  inFlight = true;
  check(openKick(A, B, &inFlight) && !inFlight && A.outbox.size() == a0 + 1 &&
        sessOf(A)->state == SESSION_ESTABLISHING,
        "C17 a kick on an ACTIVE session starts the explicit re-handshake");
  deliver(take(A), B);
  deliver(take(B), A);
  check(keysMirror() && sessOf(A)->sessionId != idBefore, "C17 which completes on a new sessionId");

  // C18 — an OPEN that never gets its CONFIRM: the timeout sweep frees the
  // slot and the next kick starts over (preserved).
  resetDevices();
  openKick(A, B, &inFlight);
  take(A);
  gNow += kSessionEstablishTimeoutMs;
  use(A);
  check(sessionEstablishingTimeoutSweep(gNow) == 1 && sessOf(A) == nullptr,
        "C18 the sweep frees an ESTABLISHING slot after the timeout");
  inFlight = true;
  check(openKick(A, B, &inFlight) && !inFlight && A.outbox.size() == 2,
        "C18 the next kick sends a new SESSION_OPEN");
  deliver(take(A), B);
  deliver(take(B), A);
  check(keysMirror(), "C18 and the retry completes");

  // C19 — the same without the sweep (its tick held off): the slot counts as
  // in flight up to the timeout and as stale from then on.
  resetDevices();
  openKick(A, B, &inFlight);
  o1 = take(A);
  idBefore = sessOf(A)->sessionId;
  gNow += kSessionEstablishTimeoutMs - 1;
  inFlight = false;
  openKick(A, B, &inFlight);
  check(inFlight && A.outbox.size() == 1 && sessOf(A)->sessionId == idBefore,
        "C19 one millisecond before the timeout a kick still leaves the handshake alone (FAILS ON BASE)");
  gNow += 1;
  inFlight = true;
  check(openKick(A, B, &inFlight) && !inFlight && A.outbox.size() == 2 &&
        sessOf(A)->state == SESSION_ESTABLISHING && sessOf(A)->sessionId != idBefore,
        "C19 at the timeout a kick replaces the stale handshake");
  deliver(take(A), B);
  deliver(take(B), A);
  check(keysMirror(), "C19 and the new one completes");
  use(B);
  std::memset(B.table, 0, sizeof B.table);
  deliver(o1, B);
  const Frame lateConfirm = take(B);
  deliver(lateConfirm, A);
  check(sessOf(A)->state == SESSION_ACTIVE && sessOf(A)->sessionId != idBefore &&
        logsContaining(A, "WARN", "no in-flight session") == 1,
        "C19 a CONFIRM for the abandoned OPEN is refused and the live session kept");

  // C20 — the bench glare (2026-11-18): A's OPEN went out while B could not
  // hear it (B was restarting) and is lost; B then opens. Before, A ignored
  // B's OPEN and both waited out the 6 s CONFIRM budget. Now A's copy reaches
  // B, B yields, and one session stands at once.
  resetDevices();
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  const Frame lostOpen = take(A);  // never delivered
  gNow += 1500;
  use(B);
  espnowSessionOpenInitiate(A.mac, "mesh");
  const Frame bOpen = take(B);
  deliver(bOpen, A);
  check(A.outbox.size() == 2 && A.outbox[1].payload == lostOpen.payload,
        "C20 A answers the glare with a copy of its lost OPEN (FAILS ON BASE)");
  deliver(bOpen, A);  // B's OPEN again (a retry): no second copy
  check(A.outbox.size() == 2, "C20 at most one copy per attempt");
  A.delivered = 1;
  exchange(6);
  V4PayloadSessionOpen lostMsg;
  std::memcpy(&lostMsg, lostOpen.payload.data(), sizeof lostMsg);
  check(keysMirror() && sessOf(B)->sessionId == lostMsg.sessionId,
        "C20 B yields to the copy; both converge on A's session without a timeout (FAILS ON BASE)");
  // A new attempt on the same slot may send its copy again.
  gNow += 100000;
  bool inFlight2 = false;
  openKick(A, B, &inFlight2);
  const Frame second2 = take(A);
  openKick(B, A, &inFlight2);
  const size_t aBefore = A.outbox.size();
  deliver(take(B), A);
  check(A.outbox.size() == aBefore + 1 && A.outbox.back().payload == second2.payload,
        "C20 a new attempt on the slot sends its copy again (FAILS ON BASE)");

  // C21 — only the A side sends a copy: B (higher MAC) yields as before.
  resetDevices();
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  use(B);
  espnowSessionOpenInitiate(A.mac, "mesh");
  const size_t bBefore = B.outbox.size();
  deliver(take(A), B);
  check(B.outbox.size() == bBefore + 1 && B.outbox.back().type == ESPNOW_V4_TYPE_SESSION_CONFIRM,
        "C21 the B side answers A's OPEN with a CONFIRM and sends no OPEN copy");

  // ---- The session capability word (1.0) -------------------------------------
  // "FAILS WITHOUT THE RULE": a tree whose OPEN/CONFIRM carry no word, or whose
  // signed transcripts do not cover it.
#ifdef HW1_SESSION_CAPS
  // C22 — each side keeps the other's word; the initiator's view is confirmed
  // at once, the responder's after the first sealed frame of the session.
  resetDevices();
  use(A);
  check(espnowSessionOpenInitiate(B.mac, "mesh") && A.outbox.size() == 1, "C22 setup: A sends OPEN");
  deliver(take(A), B);
  deliver(take(B), A);   // no sealed frame yet (establish() would seal one each way)
  check(sessOf(A)->state == SESSION_ACTIVE && sessOf(B)->state == SESSION_ACTIVE, "C22 setup: both ACTIVE");
  {
    V4PayloadSessionOpen om{};
    V4PayloadSessionConfirm cm{};
    std::memcpy(&om, A.outbox[0].payload.data(), sizeof om);
    std::memcpy(&cm, B.outbox[0].payload.data(), sizeof cm);
    const uint8_t* op = A.outbox[0].payload.data();
    const uint8_t* cp = B.outbox[0].payload.data();
    check(A.outbox[0].payload.size() == 154 && B.outbox[0].payload.size() == 162,
          "C22 OPEN is 154 bytes and CONFIRM 162 (1.0 step 4: openEpochMs and the mesh tag)");
    check(op[62] == 0xFE && op[63] == 0 && op[64] == 0 && op[65] == 0 && om.sessionCaps == A.caps,
          "C22 OPEN carries A's word, little endian at offset 62");
    check(cp[78] == 0xFF && cp[79] == 0 && cp[80] == 0x01 && cp[81] == 0 && cm.sessionCaps == B.caps,
          "C22 CONFIRM carries B's word, little endian at offset 78");
  }
  check(sessOf(A)->peerSessionCaps == B.caps && sessOf(A)->peerSessionCapsConfirmed == 1,
        "C22 A keeps B's word (reserved bit too), confirmed (from the CONFIRM)");
  check(sessOf(B)->peerSessionCaps == A.caps && sessOf(B)->peerSessionCapsConfirmed == 0,
        "C22 B keeps A's word, not yet confirmed (an OPEN can be a replayed copy)");
  check(sealOpen(B, A) && sessOf(B)->peerSessionCapsConfirmed == 0,
        "C22 B's own sealed frame does not confirm A's word");
  check(sealOpen(A, B) && sessOf(B)->peerSessionCapsConfirmed == 1,
        "C22 the first sealed frame from A confirms A's word on B");
  check(keysMirror(), "C22 the session works both ways");

  // C23 — the word is signed: a changed caps byte in OPEN is refused.
  resetDevices();
  use(A);
  check(espnowSessionOpenInitiate(B.mac, "mesh") && A.outbox.size() == 1, "C23 setup: A sends OPEN");
  {
    Frame f = take(A);
    f.payload[62] ^= 0x01;   // bond bit of A's word
    deliver(f, B);
    check(B.outbox.empty() && sessOf(B) == nullptr,
          "C23 an OPEN whose caps byte changed gets no CONFIRM and no slot (FAILS WITHOUT THE RULE)");
    check(logsContaining(B, "WARN", "mesh key mismatch") == 1,
          "C23 the mesh tag fails first (it covers the transcript)");
    // A sender that holds the passphrase re-tags the changed frame: the
    // signature still refuses it.
    V4PayloadSessionOpen om{};
    std::memcpy(&om, f.payload.data(), sizeof om);
    uint8_t t[kOpenTranscriptLen];
    buildOpenTranscript(t, om);
    use(A);
    sessionMeshTag(om.meshTag, espnowhs::MeshTagKind::Open, 0, t, sizeof t, om.signature);
    std::memcpy(f.payload.data(), &om, sizeof om);
    gNow += 31000;   // past the per-peer reject line interval
    deliver(f, B);
    check(B.outbox.empty() && sessOf(B) == nullptr && logsContaining(B, "WARN", "signature") == 1,
          "C23 re-tagged, the changed OPEN fails the signature");
  }

  // C24 — the same for CONFIRM: A's slot is cleared.
  resetDevices();
  use(A);
  check(espnowSessionOpenInitiate(B.mac, "mesh"), "C24 setup: A sends OPEN");
  deliver(take(A), B);
  {
    Frame f = take(B);
    check(f.type == ESPNOW_V4_TYPE_SESSION_CONFIRM, "C24 setup: B answers with CONFIRM");
    f.payload[78] ^= 0x01;
    V4PayloadSessionConfirm cm{};   // re-tagged under the mesh key: the signature is what refuses it
    std::memcpy(&cm, f.payload.data(), sizeof cm);
    uint8_t t[kConfirmTranscriptLen];
    buildConfirmTranscript(t, cm);
    use(B);
    sessionMeshTag(cm.meshTag, espnowhs::MeshTagKind::Confirm, 0, t, sizeof t, cm.signature);
    std::memcpy(f.payload.data(), &cm, sizeof cm);
    deliver(f, A);
    SessionState* sa = sessOf(A);
    check(sa == nullptr || sa->state != SESSION_ACTIVE,
          "C24 a CONFIRM whose caps byte changed does not make A ACTIVE (FAILS WITHOUT THE RULE)");
    check(logsContaining(A, "WARN", "Ed25519 verify failed") == 1, "C24 A says the signature failed");
  }

  // C25 — a REKEY keeps the words on both sides.
  resetDevices();
  check(establish(), "C25 setup: session established");
  gNow += kRekeyMinIntervalMs + 1000;
  check(rekeyRound(A, B), "C25 a rekey round completes");
  check(sessOf(A)->peerSessionCaps == B.caps && sessOf(B)->peerSessionCaps == A.caps &&
        sessOf(A)->peerSessionCapsConfirmed == 1,
        "C25 both words survive the rekey");

  // C26 — a new handshake after the peer's word changed (a reflash) stores the
  // new word; while it is in flight the slot holds no word.
  B.caps = 0x000000FFu;
  use(A);
  check(espnowSessionOpenInitiate(B.mac, "mesh"), "C26 A re-handshakes over the live session");
  check(sessOf(A)->state == SESSION_ESTABLISHING && sessOf(A)->peerSessionCaps == 0 &&
        sessOf(A)->peerSessionCapsConfirmed == 0,
        "C26 the ESTABLISHING slot keeps no word from the last session");
  deliver(take(A), B);
  deliver(take(B), A);
  check(keysMirror() && sessOf(A)->peerSessionCaps == 0xFFu, "C26 the new word replaces the old");
  B.caps = 0x000100FFu;

  // C27 — the glare re-send signs the same word, and B takes it.
  resetDevices();
  use(B);
  check(espnowSessionOpenInitiate(A.mac, "mesh"), "C27 setup: B opens");
  use(A);
  check(espnowSessionOpenInitiate(B.mac, "mesh"), "C27 setup: A opens too (glare)");
  {
    Frame lost = take(A);   // A's first OPEN is lost
    deliver(take(B), A);    // B's OPEN reaches A: A keeps its own and sends it again
    check(A.outbox.size() == 2 && A.outbox[1].type == ESPNOW_V4_TYPE_SESSION_OPEN &&
          A.outbox[1].payload == lost.payload,
          "C27 A's copy is byte for byte its first OPEN, caps included");
    deliver(take(A), B);
    deliver(take(B), A);
    check(keysMirror() && sessOf(B)->peerSessionCaps == A.caps && sessOf(A)->peerSessionCaps == B.caps,
          "C27 the copy verifies at B and both keep the other's word");
  }

  // C28 — golden transcripts (1.0 step 4): 92 and 100 bytes, hw1-espnow-1-*
  // labels, the word little endian, OPEN's openEpochMs last.
  {
    uint8_t initMac[6] = {1, 2, 3, 4, 5, 6}, respMac[6] = {7, 8, 9, 10, 11, 12};
    V4PayloadSessionOpen om{};
    V4PayloadSessionConfirm cm{};
    om.sessionId = cm.sessionId = 0xBEEF;
    std::memcpy(om.initiatorMac, initMac, 6);
    std::memcpy(om.responderMac, respMac, 6);
    std::memcpy(cm.initiatorMac, initMac, 6);
    std::memcpy(cm.responderMac, respMac, 6);
    for (int i = 0; i < 32; i++) om.ephX25519Pub[i] = cm.ephX25519Pub[i] = (uint8_t)(0x40 + i);
    for (int i = 0; i < 16; i++) {
      om.nonceA[i] = cm.nonceA[i] = (uint8_t)(0x80 + i);
      cm.nonceB[i] = (uint8_t)(0xC0 + i);
    }
    om.sessionCaps = 0x12345678u;
    om.openEpochMs = 0x0102030405060708ull;
    cm.sessionCaps = 0x9ABCDEF0u;
    uint8_t ot[kOpenTranscriptLen + 1], ct[kConfirmTranscriptLen + 1];
    std::memset(ot, 0xEE, sizeof ot);
    std::memset(ct, 0xEE, sizeof ct);
    buildOpenTranscript(ot, om);
    buildConfirmTranscript(ct, cm);
    check(kOpenTranscriptLen == 92 && kConfirmTranscriptLen == 100 && kRekeyTranscriptLen == 84,
          "C28 transcript lengths 92, 100 and 84");
    check(std::memcmp(ot, "hw1-espnow-1-sopen", 18) == 0 && ot[18] == 0xEF && ot[19] == 0xBE &&
          std::memcmp(ot + 20, initMac, 6) == 0 && std::memcmp(ot + 26, respMac, 6) == 0 &&
          std::memcmp(ot + 32, om.ephX25519Pub, 32) == 0 && std::memcmp(ot + 64, om.nonceA, 16) == 0 &&
          ot[80] == 0x78 && ot[81] == 0x56 && ot[82] == 0x34 && ot[83] == 0x12 &&
          ot[84] == 0x08 && ot[91] == 0x01 && ot[92] == 0xEE,
          "C28 OPEN transcript: label, id, MACs, eph, nonceA, caps LE at 80..83, openEpochMs LE at 84..91");
    check(std::memcmp(ct, "hw1-espnow-1-sconf", 18) == 0 && ct[18] == 0xEF && ct[19] == 0xBE &&
          std::memcmp(ct + 20, respMac, 6) == 0 && std::memcmp(ct + 26, initMac, 6) == 0 &&
          std::memcmp(ct + 32, cm.ephX25519Pub, 32) == 0 && std::memcmp(ct + 64, cm.nonceA, 16) == 0 &&
          std::memcmp(ct + 80, cm.nonceB, 16) == 0 &&
          ct[96] == 0xF0 && ct[97] == 0xDE && ct[98] == 0xBC && ct[99] == 0x9A && ct[100] == 0xEE,
          "C28 CONFIRM transcript: ... nonceB, caps LE at 96..99, nothing past 100");
  }
#else
  check(false, "the tree has no session capability word (HW1_SESSION_CAPS unset)");
#endif

#ifdef HW1_SYNC_SENDER_CHECK
  // C29 — ESP-NOW 1.0 step 3: the three session handlers check the sender on
  // espnow_task. A revoked sender, or one with no stored identity, has nothing
  // queued to cmd_exec and gets no answer.
  {
    resetDevices();
    use(A);
    check(espnowSessionOpenInitiate(B.mac, "mesh") && !A.outbox.empty(), "C29 setup: A sends an OPEN");
    const Frame open = take(A);
    gSenderRevoked = true;
    use(B);
    const size_t b0 = B.outbox.size();
    deliver(open, B);
    check(B.queue.empty() && B.outbox.size() == b0 && !sessOf(B),
          "C29 a revoked sender's SESSION_OPEN queues nothing and is not answered");
    check(logsContaining(B, "WARN", "revoked here") == 1, "C29 one line names the revoke");
    gSenderRevoked = false;
    resetDevices();
    establish();
    gNow += kRekeyAgeThresholdMs;
    use(A);
    check(espnowRekeyInitiate(B.mac), "C29 setup: A sends a REKEY");
    const Frame rk = take(A);
    gSenderRevoked = true;
    gNow += 31000;   // past the drop-line interval
    const size_t b1 = B.outbox.size();
    deliver(rk, B);
    check(B.outbox.size() == b1 && sessOf(B)->state == SESSION_ACTIVE,
          "C29 a revoked sender's SESSION_REKEY queues nothing: keys unchanged, no reply");
    gSenderRevoked = false;
    resetDevices();
    use(A);
    espnowSessionOpenInitiate(B.mac, "mesh");
    const Frame open2 = take(A);
    B.peer.valid = false;   // B no longer holds A's identity (unpaired there)
    gNow += 31000;
    deliver(open2, B);
    check(B.queue.empty() && !sessOf(B) && logsContaining(B, "WARN", "espnowunpair") >= 1,
          "C29 a sender with no stored identity is dropped before queueing; the line names the remedy");
    pairIdentities();
  }
#endif

  // ---- ESP-NOW 1.0 step 4: revocation C (sessions bound to the mesh key) ------
  // C30 — C1 by host only: B has the same mesh id but another bootstrap key
  // (A changed the passphrase, or B is a device cut off by a rotation). A's
  // OPEN is refused at B as "mesh key mismatch" before the signature; nothing
  // is answered and no slot is made.
  resetDevices();
  std::memset(B.mesh.bootstrapKey, 0x77, 32);
  use(A);
  check(espnowSessionOpenInitiate(B.mac, "mesh") && A.outbox.size() == 1, "C30 setup: A sends OPEN");
  deliver(take(A), B);
  check(B.outbox.empty() && sessOf(B) == nullptr, "C30 the OPEN gets no CONFIRM and no slot");
  check(logsContaining(B, "WARN", "mesh key mismatch") == 1 && logsContaining(B, "WARN", "signature") == 0,
        "C30 B logs 'mesh key mismatch', not a signature failure");
  // The other way round: B's OPEN (tagged under B's key) is refused at A too.
  use(B);
  check(espnowSessionOpenInitiate(A.mac, "mesh") && B.outbox.size() == 1, "C30 setup: B sends OPEN");
  deliver(take(B), A);
  check(A.outbox.size() == 1 && logsContaining(A, "WARN", "mesh key mismatch") == 1,
        "C30 A refuses B's OPEN as well (A's own OPEN is the only frame A sent)");
  std::memset(B.mesh.bootstrapKey, 0x61, 32);
  // C31 — the same passphrase again: the session opens without re-pairing.
  resetDevices();
  check(establish(), "C31 with the same mesh key the session opens (identities kept)");
  // C32 — C2: the session keys depend on the bootstrap key (same shared secret).
  {
    uint8_t shared[32], k1[32], k2[32], k3[32], k4[32], t1[16], t2[16];
    std::memset(shared, 0x33, 32);
    uint8_t other[32];
    std::memset(other, 0x77, 32);
    check(sessionDeriveKeyMaterial(A.mesh.bootstrapKey, shared, k1, k2, t1) &&
              sessionDeriveKeyMaterial(other, shared, k3, k4, t2) &&
              std::memcmp(k1, k3, 32) != 0 && std::memcmp(k2, k4, 32) != 0 && std::memcmp(t1, t2, 16) != 0,
          "C32 another bootstrap key gives other session keys and another bond token");
    A.mesh.valid = false;   // a mesh without a passphrase: no session on it
    resetDevices();
    use(A);
    check(!espnowSessionOpenInitiate(B.mac, "mesh") && A.outbox.empty(),
          "C32 no SESSION_OPEN on a mesh without a key");
    A.mesh.valid = true;
  }
  // C33 — openEpochMs: OPEN carries the signer's clock (0 when not set), and it
  // is signed (a re-tagged OPEN with another time fails the signature).
  resetDevices();
  Clock::gEpochMs = 0;
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  {
    V4PayloadSessionOpen om{};
    std::memcpy(&om, A.outbox.back().payload.data(), sizeof om);
    check(om.openEpochMs == 0, "C33 an unset clock sends openEpochMs 0");
  }
  resetDevices();
  Clock::gEpochMs = 1800000000123LL;
  use(A);
  espnowSessionOpenInitiate(B.mac, "mesh");
  {
    Frame f = take(A);
    V4PayloadSessionOpen om{};
    std::memcpy(&om, f.payload.data(), sizeof om);
    check(om.openEpochMs == 1800000000123ull, "C33 a set clock is sent as openEpochMs");
    om.openEpochMs += 1;
    uint8_t t[kOpenTranscriptLen];
    buildOpenTranscript(t, om);
    sessionMeshTag(om.meshTag, espnowhs::MeshTagKind::Open, 0, t, sizeof t, om.signature);
    std::memcpy(f.payload.data(), &om, sizeof om);
    deliver(f, B);
    check(B.outbox.empty() && logsContaining(B, "WARN", "signature") == 1,
          "C33 a changed openEpochMs fails the signature");
  }

  check(gGuardDepth == 0, "no lock left held");
  std::printf("%u failure(s)\n", gFailures);
  return gFailures ? 1 : 0;
}
