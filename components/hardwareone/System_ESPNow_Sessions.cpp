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

bool sessionWrapFrame(SessionState* s,
                      struct EspNowV4Header* headerInOut,
                      const uint8_t* plaintext, uint16_t plaintextLen,
                      uint8_t* outCipherWithTag) {
  if (!s || s->state != SESSION_ACTIVE) return false;
  if (!headerInOut || !plaintext || !outCipherWithTag) return false;
  if (s->txSeqNext == 0xFFFFFFFFu) {
    // Refuse to wrap around — caller should rekey via Phase 3.6's REKEY path
    // (not yet implemented; for now this is just a hard stop).
    WARN_ESPNOWF("session: txSeqNext exhausted (sessionId=%u) — rekey needed",
                 (unsigned)s->sessionId);
    return false;
  }

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
  if (!s || s->state != SESSION_ACTIVE) return false;
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
  if (!espnowCryptoAeadOpen(outPlain, cipher, cipherLen, tag,
                            aad, 30, nonce, s->aeadKeyRx)) {
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

#endif  // ENABLE_ESPNOW
