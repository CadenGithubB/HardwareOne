#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Sessions.h"

#include <Arduino.h>
#include <string.h>
#include <sodium.h>

#include "System_Debug.h"
#include "System_ESPNow_Crypto.h"
#include "System_ESPNow_Wire.h"  // EspNowV4Header, flag constants
#include "System_MemUtil.h"      // ps_alloc, AllocPref
#include "System_Mutex.h"        // EspNowTxGuard — serializes the session TX critical section

namespace {

constexpr uint8_t kSessionSlots = 16;
SessionState* gSessions = nullptr;

}  // namespace

bool sessionsInit() {
  if (gSessions) return true;
  gSessions = (SessionState*)ps_alloc(sizeof(SessionState) * kSessionSlots,
                                      AllocPref::PreferPSRAM, "espnow.sessions");
  if (!gSessions) {
    ERROR_ESPNOWF("sessions: failed to allocate %u-slot table",
                  (unsigned)kSessionSlots);
    return false;
  }
  memset(gSessions, 0, sizeof(SessionState) * kSessionSlots);
  return true;
}

SessionState* sessionFindByPeer(const uint8_t peerMac[6], uint8_t meshId) {
  if (!gSessions) return nullptr;
  for (uint8_t i = 0; i < kSessionSlots; i++) {
    SessionState& s = gSessions[i];
    if (s.state != SESSION_FREE &&
        s.meshId == meshId &&
        memcmp(s.peerMac, peerMac, 6) == 0) {
      return &s;
    }
  }
  return nullptr;
}

SessionState* sessionFindBySessionId(uint16_t sessionId, const uint8_t peerMac[6]) {
  if (!gSessions || sessionId == 0) return nullptr;
  for (uint8_t i = 0; i < kSessionSlots; i++) {
    SessionState& s = gSessions[i];
    if (s.state != SESSION_FREE &&
        s.sessionId == sessionId &&
        memcmp(s.peerMac, peerMac, 6) == 0) {
      return &s;
    }
  }
  return nullptr;
}

SessionState* sessionAllocate(const uint8_t peerMac[6], uint8_t meshId) {
  if (!gSessions) {
    if (!sessionsInit()) return nullptr;
  }
  // Reuse existing slot for this peer/mesh if present.
  SessionState* existing = sessionFindByPeer(peerMac, meshId);
  if (existing) {
    sodium_memzero(existing->aeadKeyTx, sizeof(existing->aeadKeyTx));
    sodium_memzero(existing->aeadKeyRx, sizeof(existing->aeadKeyRx));
    existing->state = SESSION_ESTABLISHING;
    existing->txSeqNext = 0;
    existing->rxSeqHighWater = 0;
    existing->rxSeqBitmap = 0;
    existing->establishedAtMs = 0;
    existing->lastUseMs = (uint32_t)millis();
    return existing;
  }
  // Otherwise grab a free slot.
  for (uint8_t i = 0; i < kSessionSlots; i++) {
    SessionState& s = gSessions[i];
    if (s.state == SESSION_FREE) {
      memset(&s, 0, sizeof(s));
      memcpy(s.peerMac, peerMac, 6);
      s.meshId = meshId;
      s.state = SESSION_ESTABLISHING;
      s.lastUseMs = (uint32_t)millis();
      return &s;
    }
  }
  ERROR_ESPNOWF("sessions: no free slots (cap=%u)", (unsigned)kSessionSlots);
  return nullptr;
}

void sessionClear(SessionState* s) {
  if (!s) return;
  sodium_memzero(s->aeadKeyTx, sizeof(s->aeadKeyTx));
  sodium_memzero(s->aeadKeyRx, sizeof(s->aeadKeyRx));
  memset(s, 0, sizeof(*s));
  s->state = SESSION_FREE;
}

bool sessionIsASide(const uint8_t myMac[6], const uint8_t peerMac[6]) {
  return memcmp(myMac, peerMac, 6) < 0;
}

bool sessionDeriveAeadKeys(SessionState* s, const uint8_t shared[32]) {
  if (!s) return false;
  uint8_t keyAtoB[32], keyBtoA[32];
  bool ok1 = espnowCryptoKdfSubkey(keyAtoB, shared, 1, "esp-AtoB");
  bool ok2 = espnowCryptoKdfSubkey(keyBtoA, shared, 2, "esp-BtoA");
  if (!ok1 || !ok2) {
    sodium_memzero(keyAtoB, sizeof(keyAtoB));
    sodium_memzero(keyBtoA, sizeof(keyBtoA));
    return false;
  }
  if (s->myDirection == 0) {
    // We are A: encrypt with AtoB, decrypt with BtoA
    memcpy(s->aeadKeyTx, keyAtoB, 32);
    memcpy(s->aeadKeyRx, keyBtoA, 32);
  } else {
    memcpy(s->aeadKeyTx, keyBtoA, 32);
    memcpy(s->aeadKeyRx, keyAtoB, 32);
  }
  sodium_memzero(keyAtoB, sizeof(keyAtoB));
  sodium_memzero(keyBtoA, sizeof(keyBtoA));

  // Derive the bond auth token (subkey id 3) from the SAME shared secret, for
  // EVERY session regardless of bond-mode state. This decouples bond-token
  // existence from the order in which a session is established vs. bond mode is
  // enabled: previously the token was only derived when bondModeEnabled was
  // already true at handshake time, so bonding on top of an existing session
  // (e.g. mesh-paired first, then bondconnect) left the token absent forever.
  // Only the configured bond peer's token is ever consulted (see
  // bondPeerActiveSession), so deriving it for all peers is cheap and harmless.
  {
    uint8_t bondTok[32];
    if (espnowCryptoKdfSubkey(bondTok, shared, 3, "esp-bond")) {
      memcpy(s->bondToken, bondTok, 16);
      s->bondTokenValid = 1;
    }
    sodium_memzero(bondTok, sizeof(bondTok));
  }
  return true;
}

const SessionState* sessionAt(uint8_t i) {
  if (!gSessions || i >= kSessionSlots) return nullptr;
  const SessionState& s = gSessions[i];
  return (s.state == SESSION_FREE) ? nullptr : &s;
}

uint8_t sessionSlotCount() {
  return kSessionSlots;
}

// ============================================================================
// Phase 3.5a — frame wrap / unwrap + replay window
// ============================================================================
//
// Nonce construction (12 bytes for ChaCha20-Poly1305 IETF):
//   [0..1]  sessionId   (uint16 LE)
//   [2]     direction   (0 = A→B traffic, 1 = B→A traffic)
//   [3..6]  frameSeq    (uint32 LE)
//   [7..11] zero
// Direction is fixed per session — we always TX with our own direction byte,
// receive frames with the opposite direction. The direction-byte difference
// makes nonce-reuse between A's TX and B's TX impossible even if they pick
// the same frameSeq.
//
// AAD = first 30 bytes of the V4 header (everything except the crc16 field
// at offsets 30–31, which is set to zero on SESSION_FRAME frames anyway).
// Binding AAD to the tag stops in-flight header tampering (msgId, type,
// flags, fragment fields) without us having to defend each field separately.

namespace {

constexpr size_t kAeadTagBytes = 16;

void buildNonce(uint8_t outNonce[12],
                uint16_t sessionId,
                uint8_t direction,
                uint32_t frameSeq) {
  outNonce[0]  = (uint8_t)(sessionId & 0xFF);
  outNonce[1]  = (uint8_t)((sessionId >> 8) & 0xFF);
  outNonce[2]  = direction;
  outNonce[3]  = (uint8_t)(frameSeq & 0xFF);
  outNonce[4]  = (uint8_t)((frameSeq >> 8) & 0xFF);
  outNonce[5]  = (uint8_t)((frameSeq >> 16) & 0xFF);
  outNonce[6]  = (uint8_t)((frameSeq >> 24) & 0xFF);
  outNonce[7]  = 0;
  outNonce[8]  = 0;
  outNonce[9]  = 0;
  outNonce[10] = 0;
  outNonce[11] = 0;
}

// Replay-window check + insert. Returns true to accept the frame.
//   Strictly newer frame  → shift window, advance highWater, accept.
//   Within 64-frame window → check bitmap; accept once, reject duplicate.
//   Below window low edge → reject (too old).
bool replayCheckAndInsert(SessionState* s, uint32_t frameSeq) {
  if (frameSeq == 0) {
    // We never send frameSeq=0 (txSeqNext starts at 0 and ++s to 1 on first
    // send), but reject defensively in case a peer is misbehaving.
    return false;
  }
  if (s->rxSeqHighWater == 0) {
    // First frame on this session — accept and seed.
    s->rxSeqHighWater = frameSeq;
    s->rxSeqBitmap    = 1ULL;  // bit 0 = this very frame
    return true;
  }
  if (frameSeq > s->rxSeqHighWater) {
    uint32_t shift = frameSeq - s->rxSeqHighWater;
    if (shift >= 64) {
      s->rxSeqBitmap = 1ULL;
    } else {
      s->rxSeqBitmap = (s->rxSeqBitmap << shift) | 1ULL;
    }
    s->rxSeqHighWater = frameSeq;
    return true;
  }
  uint32_t diff = s->rxSeqHighWater - frameSeq;
  if (diff >= 64) return false;  // too old
  uint64_t mask = 1ULL << diff;
  if (s->rxSeqBitmap & mask) return false;  // duplicate
  s->rxSeqBitmap |= mask;
  return true;
}

}  // namespace

// Phase 5 follow-up: tag each failure path so the caller's log
// ("sessionWrapFrame failed") is no longer opaque. A field-test fallback
// from v4_send_payload_smart didn't tell us which branch fired — these
// WARNs close that gap. State enum names mirror SessionStateLifecycle:
//   FREE=0, ESTABLISHING=1, ACTIVE=2, REKEYING=3, CLOSED=4.
static const char* kSessionStateNames[] = {
  "FREE", "ESTABLISHING", "ACTIVE", "REKEYING", "CLOSED"
};
static const char* sessionStateName(uint8_t s) {
  return (s < sizeof(kSessionStateNames) / sizeof(kSessionStateNames[0]))
           ? kSessionStateNames[s] : "?";
}

bool sessionWrapFrame(SessionState* s,
                      struct EspNowV4Header* headerInOut,
                      const uint8_t* plaintext, uint16_t plaintextLen,
                      uint8_t* outCipherWithTag) {
  if (!s) {
    WARN_ESPNOWF("session: wrap reject — null session pointer");
    return false;
  }
  if (s->state != SESSION_ACTIVE) {
    // Most common cause for an intermittent fallback: caller looked up the
    // session and saw ACTIVE, but by the time wrap runs the state flipped
    // (REKEYING during a rekey round, CLOSED if peer tore down, etc.).
    WARN_ESPNOWF("session: wrap reject (sessionId=%u) — state=%s (need ACTIVE)",
                 (unsigned)s->sessionId, sessionStateName(s->state));
    return false;
  }
  // `plaintext` is allowed to be NULL when `plaintextLen == 0` — some opcodes
  // (METADATA_REQ, app-ping HEARTBEAT) have an empty payload; the AEAD seal
  // still produces a 16-byte authenticator over the AAD, which is what the
  // receiver verifies. libsodium's chacha20poly1305 ietf_encrypt_detached
  // explicitly accepts (NULL, 0) for the plaintext input.
  if (!headerInOut || !outCipherWithTag ||
      (plaintextLen > 0 && !plaintext)) {
    WARN_ESPNOWF("session: wrap reject (sessionId=%u) — null arg (hdr=%p pt=%p len=%u out=%p)",
                 (unsigned)s->sessionId,
                 (const void*)headerInOut, (const void*)plaintext,
                 (unsigned)plaintextLen,
                 (const void*)outCipherWithTag);
    return false;
  }
  if (s->txSeqNext == 0xFFFFFFFFu) {
    // Refuse to wrap the seq counter around. In practice the Phase 3.6
    // auto-rekey threshold (txSeq >= 10k, see kRekeyTxFramesThreshold) rotates
    // keys long before this — a fresh session after REKEY resets txSeqNext to
    // 0. This is the last-resort hard stop if a session somehow reached seq
    // exhaustion without rekeying.
    WARN_ESPNOWF("session: wrap reject (sessionId=%u) — txSeqNext exhausted (rekey needed)",
                 (unsigned)s->sessionId);
    return false;
  }

  // Serialize the nonce-counter bump + AEAD seal against every other session
  // sender. Before bonded sensor streaming, espnow_task was the sole sender so
  // this was race-free by construction; now SENSOR_BCAST_TASK (core 1, 10 Hz)
  // and cmd_exec_task also reach here. Without this, two cores racing the
  // ++txSeqNext below would mint duplicate frameSeq values → nonce reuse.
  // The guard holds only for the bump+seal (microseconds, no blocking calls).
  EspNowTxGuard txGuard("sessionWrap");

  uint32_t mySeq = ++s->txSeqNext;  // first frame uses seq=1

  // Stamp the header fields the AEAD binds to.
  headerInOut->sessionId = s->sessionId;
  headerInOut->frameSeq  = mySeq;
  headerInOut->flags    |= ESPNOW_V4_FLAG_SESSION_FRAME;
  headerInOut->crc16     = 0;

  uint8_t nonce[12];
  buildNonce(nonce, s->sessionId, s->myDirection, mySeq);

  // AAD = first 30 bytes of header (everything except crc16). Cast through
  // uint8_t* — header is __packed so layout is stable.
  const uint8_t* aad = reinterpret_cast<const uint8_t*>(headerInOut);

  // Tag goes after the ciphertext in the payload region: [cipher(N) || tag(16)]
  uint8_t* cipherOut = outCipherWithTag;
  uint8_t* tagOut    = outCipherWithTag + plaintextLen;
  if (!espnowCryptoAeadSeal(cipherOut, tagOut,
                            plaintext, plaintextLen,
                            aad, 30,
                            nonce, s->aeadKeyTx)) {
    // Roll the seq counter back — we didn't actually send anything.
    s->txSeqNext = mySeq - 1;
    ERROR_ESPNOWF("session: AEAD seal failed (sessionId=%u seq=%lu)",
                  (unsigned)s->sessionId, (unsigned long)mySeq);
    return false;
  }
  s->lastUseMs = (uint32_t)millis();
  return true;
}

bool sessionUnwrapFrame(SessionState* s,
                        const struct EspNowV4Header* header,
                        const uint8_t* cipherWithTag, uint16_t payloadLen,
                        uint8_t* outPlain, uint16_t* outPlainLen) {
  if (!s || (s->state != SESSION_ACTIVE && s->state != SESSION_REKEYING)) return false;
  if (!header || !cipherWithTag || !outPlain || !outPlainLen) return false;
  if (payloadLen < kAeadTagBytes) return false;
  uint16_t cipherLen = (uint16_t)(payloadLen - kAeadTagBytes);
  const uint8_t* cipher = cipherWithTag;
  const uint8_t* tag    = cipherWithTag + cipherLen;

  // Peer's direction = opposite of ours.
  uint8_t peerDir = (uint8_t)(s->myDirection ^ 1);
  uint8_t nonce[12];
  buildNonce(nonce, s->sessionId, peerDir, header->frameSeq);

  const uint8_t* aad = reinterpret_cast<const uint8_t*>(header);
  bool openedWithCurrent = espnowCryptoAeadOpen(outPlain, cipher, cipherLen, tag,
                                                aad, 30, nonce, s->aeadKeyRx);
  // Phase 3.6 — if the current key fails AND we have a recent prev key in the
  // retention window, try it. This catches frames sent by the peer under the
  // old key before they processed our REKEY-driven key swap.
  if (!openedWithCurrent && s->prevKeysValidUntilMs != 0 &&
      (uint32_t)millis() <= s->prevKeysValidUntilMs) {
    if (espnowCryptoAeadOpen(outPlain, cipher, cipherLen, tag,
                             aad, 30, nonce, s->aeadKeyRxPrev)) {
      INFO_ESPNOWF("session: decrypted under prev key (sessionId=%u seq=%lu) — "
                   "peer hadn't applied REKEY yet",
                   (unsigned)s->sessionId, (unsigned long)header->frameSeq);
      // Treat as success — fall through to replay check.
      openedWithCurrent = true;
    }
  }
  if (!openedWithCurrent) {
    WARN_ESPNOWF("session: AEAD open failed (sessionId=%u seq=%lu) — "
                 "tampered, wrong key, or wrong direction",
                 (unsigned)s->sessionId, (unsigned long)header->frameSeq);
    return false;
  }
  // AEAD verified; now run replay check.
  if (!replayCheckAndInsert(s, header->frameSeq)) {
    WARN_ESPNOWF("session: replay reject (sessionId=%u seq=%lu hwm=%lu)",
                 (unsigned)s->sessionId,
                 (unsigned long)header->frameSeq,
                 (unsigned long)s->rxSeqHighWater);
    // Don't expose the plaintext that we just decrypted — wipe it.
    sodium_memzero(outPlain, cipherLen);
    return false;
  }
  *outPlainLen = cipherLen;
  s->lastUseMs = (uint32_t)millis();
  return true;
}

// ============================================================================
// Phase 3.5 step 3 — pending-frame queue
// ============================================================================

// Forward decl into System_ESPNow.cpp. We call this to wrap+send a drained
// frame; it does the session lookup, AEAD seal, and esp_now_send for us.
extern bool v4_send_session_wrapped(const uint8_t dst[6], uint8_t type,
                                    uint16_t baseFlags, uint32_t msgId,
                                    const uint8_t* plaintext, uint16_t plaintextLen,
                                    uint8_t ttl, char* errOut, size_t errOutLen);

namespace {

constexpr uint8_t kPendingFrameSlots = 4;

struct PendingFrame {
  bool     inUse;
  uint8_t  peerMac[6];
  uint8_t  type;
  uint16_t flags;
  uint32_t msgId;
  uint8_t  ttl;
  uint16_t plaintextLen;
  uint32_t queuedAtMs;
  uint8_t  plaintext[kPendingFramePlaintextMax];
};

PendingFrame* gPending = nullptr;

bool ensurePendingAllocated() {
  if (gPending) return true;
  gPending = (PendingFrame*)ps_alloc(sizeof(PendingFrame) * kPendingFrameSlots,
                                     AllocPref::PreferPSRAM, "espnow.pendframes");
  if (!gPending) {
    ERROR_ESPNOWF("pending: failed to allocate %u-slot table", (unsigned)kPendingFrameSlots);
    return false;
  }
  memset(gPending, 0, sizeof(PendingFrame) * kPendingFrameSlots);
  return true;
}

}  // namespace

bool pendingFrameQueue(const uint8_t peerMac[6], uint8_t type, uint16_t flags,
                       uint32_t msgId, uint8_t ttl,
                       const uint8_t* plaintext, uint16_t plaintextLen) {
  if (!peerMac || (plaintextLen > 0 && !plaintext)) return false;
  if (plaintextLen > kPendingFramePlaintextMax) {
    ERROR_ESPNOWF("pending: plaintext %u > max %u",
                  (unsigned)plaintextLen, (unsigned)kPendingFramePlaintextMax);
    return false;
  }
  // Serialize the lazy table alloc + slot scan/claim against the drain and the
  // timeout sweep (which run on espnow_task / cmd_exec_task). Same lock as the
  // session seal; this path does no blocking work, so the hold is brief.
  EspNowTxGuard g("pendQueue");

  if (!ensurePendingAllocated()) return false;

  // Eviction policy: if a slot already holds a pending frame for this peer,
  // overwrite it (newest wins) — typical case is the app re-sending while the
  // previous one is still mid-handshake. Otherwise grab any free slot.
  PendingFrame* slot = nullptr;
  for (uint8_t i = 0; i < kPendingFrameSlots; i++) {
    PendingFrame& p = gPending[i];
    if (p.inUse && memcmp(p.peerMac, peerMac, 6) == 0) {
      WARN_ESPNOWF("pending: evicting older pending frame for %02X:%02X:%02X:%02X:%02X:%02X "
                   "(msgId=%lu→%lu)",
                   peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
                   (unsigned long)p.msgId, (unsigned long)msgId);
      slot = &p;
      break;
    }
  }
  if (!slot) {
    for (uint8_t i = 0; i < kPendingFrameSlots; i++) {
      if (!gPending[i].inUse) { slot = &gPending[i]; break; }
    }
  }
  if (!slot) {
    ERROR_ESPNOWF("pending: queue full (%u slots), dropping msgId=%lu",
                  (unsigned)kPendingFrameSlots, (unsigned long)msgId);
    return false;
  }
  slot->inUse        = true;
  memcpy(slot->peerMac, peerMac, 6);
  slot->type         = type;
  slot->flags        = flags;
  slot->msgId        = msgId;
  slot->ttl          = ttl;
  slot->plaintextLen = plaintextLen;
  slot->queuedAtMs   = (uint32_t)millis();
  if (plaintextLen) memcpy(slot->plaintext, plaintext, plaintextLen);
  return true;
}

void pendingFrameDrainForPeer(const uint8_t peerMac[6]) {
  if (!gPending || !peerMac) return;
  // Hold the lock across the drain. Safe because each send here is
  // v4_send_session_wrapped — a SINGLE frame (≤202 B), which seals + queues to
  // the radio and returns WITHOUT waiting for an ACK. So we never block on
  // espnow_task while holding the lock (no ACK-wait deadlock), and the inner
  // sessionWrapFrame take is a same-task reentrant no-op.
  EspNowTxGuard g("pendDrain");
  for (uint8_t i = 0; i < kPendingFrameSlots; i++) {
    PendingFrame& p = gPending[i];
    if (!p.inUse || memcmp(p.peerMac, peerMac, 6) != 0) continue;
    char err[96] = {0};
    bool ok = v4_send_session_wrapped(p.peerMac, p.type, p.flags, p.msgId,
                                      p.plaintext, p.plaintextLen, p.ttl,
                                      err, sizeof(err));
    if (ok) {
      INFO_ESPNOWF("pending: drained msgId=%lu to %02X:%02X:%02X:%02X:%02X:%02X",
                   (unsigned long)p.msgId,
                   p.peerMac[0], p.peerMac[1], p.peerMac[2], p.peerMac[3], p.peerMac[4], p.peerMac[5]);
    } else {
      ERROR_ESPNOWF("pending: drain failed for msgId=%lu to %02X:%02X:%02X:%02X:%02X:%02X — %s",
                    (unsigned long)p.msgId,
                    p.peerMac[0], p.peerMac[1], p.peerMac[2], p.peerMac[3], p.peerMac[4], p.peerMac[5],
                    err[0] ? err : "(no detail)");
      sendStatusMarkFailed(p.msgId, p.peerMac);
    }
    // Wipe the slot regardless — we don't auto-retry.
    sodium_memzero(p.plaintext, p.plaintextLen);
    memset(&p, 0, sizeof(p));
  }
}

uint8_t pendingFrameTimeoutSweep(uint32_t nowMs) {
  if (!gPending) return 0;
  // Same lock as queue/drain — serializes the slot scan + free against a
  // concurrent enqueue. No sends here, so the hold is a brief scan.
  EspNowTxGuard g("pendSweep");
  uint8_t expired = 0;
  for (uint8_t i = 0; i < kPendingFrameSlots; i++) {
    PendingFrame& p = gPending[i];
    if (!p.inUse) continue;
    if ((nowMs - p.queuedAtMs) < kPendingFrameTimeoutMs) continue;
    WARN_ESPNOWF("pending: timeout msgId=%lu to %02X:%02X:%02X:%02X:%02X:%02X "
                 "(no SESSION_CONFIRM in %ums) — dropping",
                 (unsigned long)p.msgId,
                 p.peerMac[0], p.peerMac[1], p.peerMac[2], p.peerMac[3], p.peerMac[4], p.peerMac[5],
                 (unsigned)kPendingFrameTimeoutMs);
    // Phase 3.5 task #49 — propagate the drop into the tracked-send table
    // so the web UI flips the bubble to ✗ Failed instead of hanging at ✓ Sent.
    sendStatusMarkFailed(p.msgId, p.peerMac);
    sodium_memzero(p.plaintext, p.plaintextLen);
    memset(&p, 0, sizeof(p));
    expired++;
  }
  return expired;
}

// A session stuck in ESTABLISHING means our SESSION_OPEN never got a CONFIRM
// (the peer was offline/booting when we kicked it). Left alone, the slot stays
// ESTABLISHING forever and v4_send_encrypted_or_queue only re-kicks a fresh
// SESSION_OPEN from FREE/CLOSED — so the bond silently never reconnects. Reset
// stale ESTABLISHING slots to FREE so the next encrypted/bond send re-initiates.
//   * lastUseMs is stamped when the slot enters ESTABLISHING and is NOT bumped
//     again until the session goes ACTIVE, so it accurately ages the handshake.
//   * Only ESTABLISHING is swept — REKEYING still holds valid current keys and
//     has its own abort path; a healthy OPEN->CONFIRM completes in well under
//     this window, so this never disturbs a live handshake.
static constexpr uint32_t kSessionEstablishTimeoutMs = 6000;  // > pending-frame 5s

uint8_t sessionEstablishingTimeoutSweep(uint32_t nowMs) {
  if (!gSessions) return 0;
  static uint32_t sLastEstabWarnMs = 0;  // throttle: a re-keyed peer times out every 6s forever
  uint8_t reset = 0;
  for (uint8_t i = 0; i < kSessionSlots; i++) {
    SessionState& s = gSessions[i];
    if (s.state != SESSION_ESTABLISHING) continue;
    if ((nowMs - s.lastUseMs) < kSessionEstablishTimeoutMs) continue;
    if (logCooldownOk(sLastEstabWarnMs, 30000)) {
      WARN_ESPNOWF("session: ESTABLISHING timeout for %02X:%02X:%02X:%02X:%02X:%02X "
                   "(no SESSION_CONFIRM in %ums) — resetting so the next send re-kicks SESSION_OPEN",
                   s.peerMac[0], s.peerMac[1], s.peerMac[2], s.peerMac[3], s.peerMac[4], s.peerMac[5],
                   (unsigned)kSessionEstablishTimeoutMs);
    }
    sessionClear(&s);
    reset++;
  }
  return reset;
}

uint8_t pendingFrameCount() {
  if (!gPending) return 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < kPendingFrameSlots; i++) {
    if (gPending[i].inUse) n++;
  }
  return n;
}

bool pendingFrameHasForPeer(const uint8_t peerMac[6]) {
  if (!gPending || !peerMac) return false;
  for (uint8_t i = 0; i < kPendingFrameSlots; i++) {
    if (gPending[i].inUse && memcmp(gPending[i].peerMac, peerMac, 6) == 0) return true;
  }
  return false;
}

// ============================================================================
// Phase 3.5 task #49 — tracked-send status (web-UI delivery confirmation)
// ============================================================================

namespace {

constexpr uint8_t kSendStatusSlots = 16;
SendStatus* gSendStatus = nullptr;

bool ensureSendStatusAllocated() {
  if (gSendStatus) return true;
  gSendStatus = (SendStatus*)ps_alloc(sizeof(SendStatus) * kSendStatusSlots,
                                      AllocPref::PreferPSRAM, "espnow.sendstatus");
  if (!gSendStatus) {
    ERROR_ESPNOWF("sendstatus: failed to allocate %u-slot table", (unsigned)kSendStatusSlots);
    return false;
  }
  memset(gSendStatus, 0, sizeof(SendStatus) * kSendStatusSlots);
  return true;
}

SendStatus* findByMsgId(uint32_t msgId) {
  if (!gSendStatus || msgId == 0) return nullptr;
  for (uint8_t i = 0; i < kSendStatusSlots; i++) {
    if (gSendStatus[i].inUse && gSendStatus[i].msgId == msgId) {
      return &gSendStatus[i];
    }
  }
  return nullptr;
}

// Pick a slot for a new registration: prefer FREE; else evict the oldest
// resolved entry (DELIVERED/TIMEOUT/FAILED); else evict the oldest entry
// overall. Newest-wins eviction policy keeps recent activity visible.
SendStatus* allocateSlot(uint32_t nowMs) {
  if (!gSendStatus) return nullptr;
  SendStatus* freeSlot     = nullptr;
  SendStatus* oldestResolved = nullptr;
  SendStatus* oldestAny    = nullptr;
  uint32_t resolvedAge = 0, anyAge = 0;
  for (uint8_t i = 0; i < kSendStatusSlots; i++) {
    SendStatus& s = gSendStatus[i];
    if (!s.inUse) { freeSlot = &s; continue; }
    uint32_t age = nowMs - s.registeredAtMs;
    if (s.state != SEND_STATUS_PENDING) {
      if (!oldestResolved || age > resolvedAge) { oldestResolved = &s; resolvedAge = age; }
    }
    if (!oldestAny || age > anyAge) { oldestAny = &s; anyAge = age; }
  }
  if (freeSlot)       return freeSlot;
  if (oldestResolved) return oldestResolved;
  return oldestAny;
}

}  // namespace

void sendStatusRegister(uint32_t msgId, const uint8_t peerMac[6]) {
  if (msgId == 0 || !peerMac) return;
  if (!ensureSendStatusAllocated()) return;
  uint32_t nowMs = (uint32_t)millis();
  // De-dup: if msgId already exists, just refresh it (drain re-sends the
  // same msgId — we don't want two entries for one logical send).
  SendStatus* existing = findByMsgId(msgId);
  SendStatus* slot = existing ? existing : allocateSlot(nowMs);
  if (!slot) return;
  slot->inUse          = true;
  slot->state          = SEND_STATUS_PENDING;
  memcpy(slot->peerMac, peerMac, 6);
  slot->msgId          = msgId;
  slot->registeredAtMs = nowMs;
  slot->resolvedAtMs   = 0;
}

// Mirror the resolved send state onto the DURABLE sent[] history record so the
// conversation keeps showing Delivered/Failed/No-ACK after this ephemeral entry
// is swept (kSendStatusKeepResolvedMs). Declared in System_ESPNow.h; forward-
// declared here to avoid pulling that heavy header into the session unit.
void espnowUpdateSentDeliveryState(const uint8_t* peerMac, uint32_t msgId, uint8_t state);

// TX-driven session liveness. Tracks consecutive unicast send-timeouts on a peer's
// ACTIVE session: a delivered ACK clears the run; each no-ACK timeout bumps it, and
// once it reaches kSessionSendTimeoutsBeforeReestablish the keys have very likely
// desynced (a handshake/REKEY frame dropped under load — nothing else tears down a
// wedged-but-"active" session). Drop it; the encrypt-or-queue send path re-handshakes
// on the next send. Driven ONLY by our own send results (no remote trigger / DoS
// surface); requiring several in a row + the 10s no-ACK window means a transient blip
// can't tear down a healthy session.
static void sessionNoteSendResult(const uint8_t peerMac[6], bool delivered) {
  if (!gSessions) return;
  for (uint8_t j = 0; j < kSessionSlots; j++) {
    SessionState& sess = gSessions[j];
    if (sess.state != SESSION_ACTIVE || memcmp(sess.peerMac, peerMac, 6) != 0) continue;
    if (delivered) {
      sess.consecutiveSendTimeouts = 0;
    } else if (++sess.consecutiveSendTimeouts >= kSessionSendTimeoutsBeforeReestablish) {
      WARN_ESPNOWF("session: %u send-timeouts in a row (sessionId=%u) — dropping suspected-"
                   "desynced session; next send re-handshakes",
                   (unsigned)sess.consecutiveSendTimeouts, (unsigned)sess.sessionId);
      sessionClear(&sess);
    }
  }
}

void sendStatusMarkDelivered(uint32_t msgId, const uint8_t srcMac[6]) {
  if (!gSendStatus || msgId == 0 || !srcMac) return;
  SendStatus* s = findByMsgId(msgId);
  if (!s || s->state != SEND_STATUS_PENDING) return;
  // Safety: confirm the ACK source matches the original dst. If they differ
  // a stale msgId from a different peer slipped in — leave the entry pending
  // rather than mark it delivered against the wrong peer.
  if (memcmp(s->peerMac, srcMac, 6) != 0) return;
  s->state        = SEND_STATUS_DELIVERED;
  s->resolvedAtMs = (uint32_t)millis();
  espnowUpdateSentDeliveryState(s->peerMac, msgId, SEND_STATUS_DELIVERED);
  sessionNoteSendResult(s->peerMac, /*delivered=*/true);  // a real ACK → session healthy, clear the run
}

void sendStatusMarkFailed(uint32_t msgId, const uint8_t peerMac[6]) {
  if (!gSendStatus || msgId == 0) return;
  SendStatus* s = findByMsgId(msgId);
  if (!s || s->state != SEND_STATUS_PENDING) return;
  if (peerMac && memcmp(s->peerMac, peerMac, 6) != 0) return;
  s->state        = SEND_STATUS_FAILED;
  s->resolvedAtMs = (uint32_t)millis();
  espnowUpdateSentDeliveryState(s->peerMac, msgId, SEND_STATUS_FAILED);
}

void sendStatusSweep(uint32_t nowMs) {
  if (!gSendStatus) return;
  for (uint8_t i = 0; i < kSendStatusSlots; i++) {
    SendStatus& s = gSendStatus[i];
    if (!s.inUse) continue;
    if (s.state == SEND_STATUS_PENDING) {
      if ((nowMs - s.registeredAtMs) >= kSendStatusPendingTimeoutMs) {
        s.state        = SEND_STATUS_TIMEOUT;
        s.resolvedAtMs = nowMs;
        espnowUpdateSentDeliveryState(s.peerMac, s.msgId, SEND_STATUS_TIMEOUT);
        // TX-driven session self-heal: count this no-ACK against the peer's ACTIVE
        // session. After kSessionSendTimeoutsBeforeReestablish in a row (a delivered
        // ACK resets it), the keys have desynced — drop it so the next send
        // re-handshakes. Only our own send failures drive this; establishing
        // sessions aren't ACTIVE yet, so an in-progress handshake is never cleared.
        sessionNoteSendResult(s.peerMac, /*delivered=*/false);
      }
    } else {
      // Resolved — free the slot after the polling-window retention period.
      if ((nowMs - s.resolvedAtMs) >= kSendStatusKeepResolvedMs) {
        memset(&s, 0, sizeof(s));
      }
    }
  }
}

bool sendStatusGet(uint32_t msgId, SendStatus* out) {
  if (!out) return false;
  SendStatus* s = findByMsgId(msgId);
  if (!s) return false;
  *out = *s;
  return true;
}

// Allow the WebPage_ESPNow handler to walk all entries for the snapshot
// JSON payload. Returns the static slot count; caller passes i = 0..N-1 and
// checks the returned slot's inUse flag.
extern "C" uint8_t sendStatusSlotCount() { return kSendStatusSlots; }
extern "C" const SendStatus* sendStatusAt(uint8_t i) {
  if (!gSendStatus || i >= kSendStatusSlots) return nullptr;
  return &gSendStatus[i];
}

// ============================================================================
// Phase 3.6 — REKEY helpers
// ============================================================================

void sessionApplyRekeyedKeys(SessionState* s,
                             const uint8_t newKeyTx[32],
                             const uint8_t newKeyRx[32]) {
  if (!s || !newKeyTx || !newKeyRx) return;
  {
    // Serialize the key + seq swap against in-flight senders. sessionWrapFrame
    // holds gEspNowSessionTxMutex while it reads aeadKeyTx and bumps txSeqNext;
    // without this guard a rekey running on cmd_exec_task could swap the TX key
    // or reset txSeqNext mid-seal on another core, producing a frame sealed
    // with a torn key/nonce pair. Same lock, so the seal and the swap are
    // mutually exclusive. Scope ends BEFORE the drain below — the drain sends
    // (re-entering sessionWrapFrame), which must not run under the held lock.
    // Section is a handful of memcpys (microseconds, no blocking calls).
    EspNowTxGuard txGuard("rekeyApply");
    // Stash current RX key as the prev — gives in-flight frames from the peer
    // (sent under the old key before they processed our REKEY) a window to
    // decrypt. We don't keep old TX keys because we control when we switch.
    memcpy(s->aeadKeyRxPrev, s->aeadKeyRx, 32);
    s->prevKeysValidUntilMs = (uint32_t)millis() + kRekeyPrevKeysWindowMs;
    // Install the new keys.
    memcpy(s->aeadKeyTx, newKeyTx, 32);
    memcpy(s->aeadKeyRx, newKeyRx, 32);
    // New keys = new nonce namespace. Reset frame counters and replay window.
    s->txSeqNext      = 0;
    s->rxSeqHighWater = 0;
    s->rxSeqBitmap    = 0;
    // Done with the rekey transient state.
    sodium_memzero(s->rekeyEphPrivKey, sizeof(s->rekeyEphPrivKey));
    s->rekeyInitiatedAtMs = 0;
    s->rekeyTxSeqAtInit   = 0;
    s->state          = SESSION_ACTIVE;
    s->establishedAtMs = (uint32_t)millis();  // age clock resets on rekey
    s->lastUseMs       = s->establishedAtMs;
  }
  // Phase 3.5 task #6 — drain any app-layer sends that got queued while we
  // were in REKEYING state (sessionWrapFrame rejects sends in that state, so
  // v4_send_encrypted_or_queue parks them in the pending-frame ring). Without
  // this drain, those frames would just expire via the 5 s timeout sweep.
  pendingFrameDrainForPeer(s->peerMac);
}

bool sessionMarkRekeyInitiated(SessionState* s,
                               const uint8_t ephPrivKey[32],
                               uint32_t txSeqAtSign) {
  if (!s || !ephPrivKey) return false;
  if (s->state != SESSION_ACTIVE) return false;
  memcpy(s->rekeyEphPrivKey, ephPrivKey, 32);
  s->rekeyInitiatedAtMs = (uint32_t)millis();
  s->rekeyTxSeqAtInit   = txSeqAtSign;
  s->state              = SESSION_REKEYING;
  return true;
}

// Phase 3.6 — the third leg of the rekey lifecycle (mark → apply | abort).
// Reverts REKEYING → ACTIVE *without* installing new keys, discarding the
// parked ephemeral private key. runDeferredRekey calls this when the ECDH or
// KDF fails after it has already entered REKEYING, so a failed rekey can no
// longer strand the session in REKEYING forever (sessionWrapFrame rejects all
// sends in that state — the prior silent `return` left the session unusable
// until reboot). The current AEAD keys were never touched, so the session
// keeps working on them. Mirrors sessionApplyRekeyedKeys' tail by draining the
// pending-frame queue: frames parked while we were briefly REKEYING can now go
// out under the still-current keys.
void sessionAbortRekey(SessionState* s) {
  if (!s || s->state != SESSION_REKEYING) return;
  sodium_memzero(s->rekeyEphPrivKey, sizeof(s->rekeyEphPrivKey));
  s->rekeyInitiatedAtMs = 0;
  s->rekeyTxSeqAtInit   = 0;
  s->state              = SESSION_ACTIVE;
  s->lastUseMs          = (uint32_t)millis();
  pendingFrameDrainForPeer(s->peerMac);
}

void sessionRekeyPrevKeysSweep(uint32_t nowMs) {
  if (!gSessions) return;
  for (uint8_t i = 0; i < kSessionSlots; i++) {
    SessionState& s = gSessions[i];
    if (s.state == SESSION_FREE) continue;
    if (s.prevKeysValidUntilMs != 0 && nowMs > s.prevKeysValidUntilMs) {
      sodium_memzero(s.aeadKeyRxPrev, sizeof(s.aeadKeyRxPrev));
      s.prevKeysValidUntilMs = 0;
    }
  }
}

#endif  // ENABLE_ESPNOW
