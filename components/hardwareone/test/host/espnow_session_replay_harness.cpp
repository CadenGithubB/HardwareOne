// Host harness: production SESSION_FRAME wrap/unwrap, replay window and REKEY
// key swap from System_ESPNow_Sessions.cpp, driven by a scripted peer pair.
// Doubles: a fake clock, counting log macros, a keyed AEAD double that binds
// key/nonce/AAD/ciphertext (libsodium is a library call, not logic under
// test), and a no-op pending-frame drain. No radio, no hardware.
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef HW1_HAS_PREV_WINDOW
#define HW1_HAS_PREV_WINDOW 0
#endif

// ---- Deadline owner (System_ESPNow_Deadline.h, when the tree has it) ---------
// INSERT_DEADLINE

// ---- Wire header (production struct, from System_ESPNow_Wire.h) ------------
// INSERT_WIRE

// ---- Session record (production struct + constants, from the header) --------
// INSERT_SESSION_TYPES
static_assert(kRekeyTxFramesThreshold > 64 && kRekeyAgeThresholdMs > kRekeyPrevKeysWindowMs &&
              kRekeyMinIntervalMs > 0 && kSessionSendTimeoutsBeforeReestablish > 0,
              "header constants referenced");

// ---- Platform doubles -------------------------------------------------------
static uint32_t gNowMs = 1000;
static unsigned long millis() { return gNowMs; }

static unsigned gWarns = 0, gInfos = 0, gErrors = 0;
static std::string gLastLog;
__attribute__((format(printf, 2, 3)))
static void noteLog(unsigned& counter, const char* fmt, ...) {
  ++counter;
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  gLastLog = buf;
}
#define WARN_ESPNOWF(...)  noteLog(gWarns, __VA_ARGS__)
#define INFO_ESPNOWF(...)  noteLog(gInfos, __VA_ARGS__)
#define ERROR_ESPNOWF(...) noteLog(gErrors, __VA_ARGS__)

static unsigned gGuardDepth = 0, gGuardTakes = 0;
struct EspNowTxGuard {
  bool held;
  explicit EspNowTxGuard(const char* = nullptr) : held(true) { ++gGuardTakes; ++gGuardDepth; }
  ~EspNowTxGuard() { --gGuardDepth; }
  EspNowTxGuard(const EspNowTxGuard&) = delete;
  EspNowTxGuard& operator=(const EspNowTxGuard&) = delete;
};
static void sodium_memzero(void* p, size_t n) { std::memset(p, 0, n); }
static unsigned gDrains = 0;
void pendingFrameDrainForPeer(const uint8_t peerMac[6]) { (void)peerMac; assert(gGuardDepth == 0); ++gDrains; }

// Keyed AEAD double. The tag is a 128-bit digest over key || nonce || aad ||
// cipher, the keystream a digest of key || nonce || index. Open succeeds only
// with the same key, nonce, AAD and ciphertext, which is the contract the
// production code relies on; it is not a cipher.
static unsigned gSeals = 0, gOpens = 0;
static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
  return h;
}
static uint64_t digest(uint64_t seed, const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t* aad, size_t aadLen, const uint8_t* body, size_t bodyLen) {
  uint64_t h = 0xcbf29ce484222325ULL ^ seed;
  h = fnv(h, key, 32);
  h = fnv(h, nonce, 12);
  h = fnv(h, aad, aadLen);
  h = fnv(h, body, bodyLen);
  return h;
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
bool espnowCryptoAeadSeal(uint8_t* cipherOut, uint8_t tagOut[16],
                          const uint8_t* plaintext, size_t plaintextLen,
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t nonce[12], const uint8_t key[32]) {
  ++gSeals;
  for (size_t i = 0; i < plaintextLen; i++) cipherOut[i] = plaintext[i] ^ keystreamByte(key, nonce, i);
  tagFor(tagOut, key, nonce, aad, aadLen, cipherOut, plaintextLen);
  return true;
}
bool espnowCryptoAeadOpen(uint8_t* plaintextOut,
                          const uint8_t* cipher, size_t cipherLen, const uint8_t tag[16],
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t nonce[12], const uint8_t key[32]) {
  ++gOpens;
  uint8_t expect[16];
  tagFor(expect, key, nonce, aad, aadLen, cipher, cipherLen);
  if (std::memcmp(expect, tag, 16) != 0) return false;
  for (size_t i = 0; i < cipherLen; i++) plaintextOut[i] = cipher[i] ^ keystreamByte(key, nonce, i);
  return true;
}

// ---- Production internals (anonymous namespace in the .cpp) -----------------
namespace {
constexpr uint8_t kSessionSlots = 2;
SessionState* gSessions = nullptr;
constexpr size_t kAeadTagBytes = 16;
// INSERT_INTERNAL
}  // namespace
static const char* sessionStateName(uint8_t s) {
  static const char* names[] = { "FREE", "ESTABLISHING", "ACTIVE", "REKEYING", "CLOSED" };
  return s < 5 ? names[s] : "?";
}
// INSERT_PRODUCTION

// ---- Scripted peers ---------------------------------------------------------
struct Frame { EspNowV4Header h; uint8_t body[64]; uint16_t bodyLen; };

static void fillKey(uint8_t key[32], uint8_t seed) { for (int i = 0; i < 32; i++) key[i] = (uint8_t)(seed * 7 + i); }

static void makeActive(SessionState& s, uint8_t dir, const uint8_t keyTx[32], const uint8_t keyRx[32]) {
  std::memset(&s, 0, sizeof s);
  s.state = SESSION_ACTIVE;
  s.sessionId = 0x1234;
  s.myDirection = dir;
  std::memcpy(s.aeadKeyTx, keyTx, 32);
  std::memcpy(s.aeadKeyRx, keyRx, 32);
  s.establishedAtMs = s.lastUseMs = (uint32_t)millis();
}

static Frame seal(SessionState& tx, const char* text) {
  Frame f;
  std::memset(&f, 0, sizeof f);
  f.h.magic = 0x3148; f.h.ver = 4; f.h.type = 50; f.h.headerLen = 32;  // type 50 = ESPNOW_V4_TYPE_CMD
  f.h.msgId = 0x1000 + tx.txSeqNext; f.h.ttl = 1; f.h.fragCount = 1;
  size_t n = std::strlen(text);
  bool ok = sessionWrapFrame(&tx, &f.h, reinterpret_cast<const uint8_t*>(text), (uint16_t)n, f.body);
  assert(ok && (f.h.flags & ESPNOW_V4_FLAG_SESSION_FRAME) && f.h.crc16 == 0);
  f.bodyLen = (uint16_t)(n + 16);
  return f;
}

static unsigned gChecks = 0, gFailures = 0;
static void check(bool ok, const char* what) {
  ++gChecks;
  if (!ok) { ++gFailures; std::fprintf(stderr, "FAIL %s\n", what); }
  else std::printf("  ok  %s\n", what);
}

// Unwrap one frame; a reject must log exactly once (WARN) and leave no plaintext.
static bool unwrap(SessionState& rx, const Frame& f, const char* expectText = nullptr) {
  uint8_t plain[64];
  std::memset(plain, 0xA5, sizeof plain);
  uint16_t len = 0;
  unsigned warnsBefore = gWarns;
  bool ok = sessionUnwrapFrame(&rx, &f.h, f.body, f.bodyLen, plain, &len);
  if (ok) {
    if (expectText) check(len == std::strlen(expectText) && std::memcmp(plain, expectText, len) == 0, "accepted frame decrypts to its plaintext");
  } else {
    check(gWarns == warnsBefore + 1, "a rejected frame logs exactly one WARN");
  }
  return ok;
}

int main() {
  uint8_t kA1[32], kB1[32], kA2[32], kB2[32], kA3[32], kB3[32];
  fillKey(kA1, 1); fillKey(kB1, 2); fillKey(kA2, 3); fillKey(kB2, 4); fillKey(kA3, 5); fillKey(kB3, 6);
  static SessionState table[kSessionSlots];
  gSessions = table;
  SessionState& A = table[0];      // receiver under test, A side
  SessionState B;                  // scripted sender, B side (its TX key is A's RX key)
  makeActive(A, 0, kA1, kB1);
  makeActive(B, 1, kB1, kA1);

  // C1 — epoch 1: 70 frames, all accepted, window advances past 64.
  std::vector<Frame> epoch1;
  bool all = true;
  for (unsigned i = 1; i <= 70; i++) { epoch1.push_back(seal(B, "epoch1")); all &= unwrap(A, epoch1.back(), "epoch1"); }
  check(all && A.rxSeqHighWater == 70, "C1 epoch-1 frames 1..70 accepted, hwm=70");
  // C2 — same-epoch replay is already rejected (duplicate and too-old).
  check(!unwrap(A, epoch1[69]) && !unwrap(A, epoch1[39]) && !unwrap(A, epoch1[4]), "C2 epoch-1 replays rejected before any rekey");
  // Late epoch-1 frames the peer sends before it applies the REKEY (captured in flight).
  Frame late71 = seal(B, "late71"), late72 = seal(B, "late72"), late73 = seal(B, "late73");

  // C3 — REKEY applied at t=10 000: fresh counters, prev key valid 5 s.
  gNowMs = 10000;
  unsigned drainsBefore = gDrains;
  sessionApplyRekeyedKeys(&A, kA2, kB2);
  check(A.state == SESSION_ACTIVE && A.txSeqNext == 0 && A.rxSeqHighWater == 0 && A.rxSeqBitmap == 0 &&
        A.prevKeysValidUntilMs == 10000 + kRekeyPrevKeysWindowMs && std::memcmp(A.aeadKeyRxPrev, kB1, 32) == 0 &&
        gDrains == drainsBefore + 1 && gGuardDepth == 0, "C3 rekey installs new keys, fresh window, prev key for 5 s");
#if HW1_HAS_PREV_WINDOW
  check(A.rxSeqHighWaterPrev == 70 && A.rxSeqBitmapPrev == ~0ULL, "C3b prev key keeps the epoch-1 window (hwm=70, full bitmap)");
#endif

  // C4 — the core regression: a recorded epoch-1 frame replayed inside the 5 s window.
  gNowMs = 10100;
  bool replayed = unwrap(A, epoch1[69]);
  check(!replayed, "C4 recorded epoch-1 frame (seq 70) is rejected after the rekey");
  check(A.rxSeqHighWater == 0 && A.rxSeqBitmap == 0, "C4b the new key's window is untouched by a prev-key frame");
  // C5/C6 — in-window duplicate and too-old epoch-1 frames.
  check(!unwrap(A, epoch1[39]), "C5 recorded epoch-1 frame (seq 40) is rejected after the rekey");
  check(!unwrap(A, epoch1[0]), "C6 recorded epoch-1 frame (seq 1, below window) is rejected after the rekey");

  // C7 — a genuinely in-flight epoch-1 frame is accepted once and never seeds the new window.
  gNowMs = 10200;
  check(unwrap(A, late71, "late71"), "C7 in-flight epoch-1 frame (seq 71) accepted once");
  check(A.rxSeqHighWater == 0 && A.rxSeqBitmap == 0, "C7b new window still empty after the late prev-key frame");
  check(!unwrap(A, late71), "C7c that late frame replayed is rejected");
#if HW1_HAS_PREV_WINDOW
  check(A.rxSeqHighWaterPrev == 71, "C7d prev window advanced to 71");
#endif

  // C8 — the peer applies the rekey; its new-epoch frames start at seq 1.
  std::memcpy(B.aeadKeyTx, kB2, 32); B.txSeqNext = 0;
  std::vector<Frame> epoch2;
  all = true;
  for (unsigned i = 1; i <= 3; i++) { epoch2.push_back(seal(B, "epoch2")); all &= unwrap(A, epoch2.back(), "epoch2"); }
  check(all && A.rxSeqHighWater == 3 && A.rxSeqBitmap == 0x7ULL, "C8 epoch-2 frames 1..3 accepted (hwm=3) after a late prev-key frame");
  check(!unwrap(A, epoch2[1]), "C9 epoch-2 replay (seq 2) rejected");

  // C10 — prev-key window expiry: sweep zeroes the key; late epoch-1 frames no longer open.
  gNowMs = 10000 + kRekeyPrevKeysWindowMs + 1;
  sessionRekeyPrevKeysSweep(gNowMs);
  bool keyZero = true;
  for (int i = 0; i < 32; i++) keyZero &= A.aeadKeyRxPrev[i] == 0;
  check(keyZero && A.prevKeysValidUntilMs == 0, "C10 sweep zeroes the expired prev key");
  check(!unwrap(A, late72), "C10b late epoch-1 frame after expiry is rejected (no prev key)");
  check(A.rxSeqHighWater == 3 && A.rxSeqBitmap == 0x7ULL, "C10c expiry and the rejected frame leave the live window alone");
#if HW1_HAS_PREV_WINDOW
  check(A.rxSeqHighWaterPrev == 0 && A.rxSeqBitmapPrev == 0, "C10d sweep zeroes the prev window too");
#endif

  // C11 — second rekey: the prev window is the epoch-2 window, not a stale epoch-1 one.
  Frame late4 = seal(B, "late4");
  gNowMs = 20000;
  sessionApplyRekeyedKeys(&A, kA3, kB3);
  check(!unwrap(A, epoch2[2]), "C11 recorded epoch-2 frame (seq 3) rejected after the second rekey");
  check(unwrap(A, late4, "late4") && A.rxSeqHighWater == 0, "C11b in-flight epoch-2 frame (seq 4) accepted once, new window untouched");
  check(!unwrap(A, late73), "C11c epoch-1 frame (key already gone) rejected");
  std::memcpy(B.aeadKeyTx, kB3, 32); B.txSeqNext = 0;
  Frame e3 = seal(B, "epoch3");
  check(unwrap(A, e3, "epoch3") && A.rxSeqHighWater == 1, "C11d epoch-3 frame seq 1 accepted");

  // C12 — unwrap keeps working while a REKEY is in flight (REKEYING state).
  uint8_t eph[32]; fillKey(eph, 9);
  check(sessionMarkRekeyInitiated(&A, eph, A.txSeqNext) && A.state == SESSION_REKEYING, "C12 session enters REKEYING");
  Frame e3b = seal(B, "epoch3b");
  check(unwrap(A, e3b, "epoch3b") && A.rxSeqHighWater == 2, "C12b frame accepted while REKEYING");
  sessionAbortRekey(&A);
  check(A.state == SESSION_ACTIVE, "C12c abort returns to ACTIVE");

  // C13 — a full clear leaves nothing behind.
  sessionClear(&A);
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&A);
  bool zero = true;
  for (size_t i = 0; i < sizeof A; i++) zero &= raw[i] == 0;
  check(zero && A.state == SESSION_FREE, "C13 sessionClear zeroes the whole record");

  check(gGuardDepth == 0 && gErrors == 0, "no guard leaked, no ERROR logged");
  std::printf("espnow_session_replay: %u checks, %u failures (seals=%u opens=%u warns=%u infos=%u)\n",
              gChecks, gFailures, gSeals, gOpens, gWarns, gInfos);
  return gFailures ? 1 : 0;
}
