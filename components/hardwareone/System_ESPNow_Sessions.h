#ifndef SYSTEM_ESPNOW_SESSIONS_H
#define SYSTEM_ESPNOW_SESSIONS_H

// ============================================================================
// ESP-NOW V4 Phase 3.4 — per-peer-per-mesh session state.
//
// A SessionState is the result of a successful SESSION_OPEN / SESSION_CONFIRM
// handshake between two peers. It holds the AEAD keys (one per direction),
// the monotonic frame counters for replay protection, and a 64-frame sliding
// replay window on the receive side. Sessions live ONLY in RAM — they vanish
// on reboot. The long-term Ed25519 identity (Phase 3.0) is what makes the
// post-reboot re-handshake safe (the peer's pubkey is still on disk, so we
// can re-verify the SESSION_OPEN signature without needing to re-pair).
//
// Phase 3.4 establishes sessions but doesn't yet enforce them on the wire —
// that's Phase 3.5's job (SESSION_FRAME wrap on send, unwrap on receive).
//
// Capacity: 16 slots (same as the existing peer table). Per-slot cost is
// well under 128 B, so ~2 KiB PSRAM total. Looked up by (peerMac, meshId) on
// the initiator path, by (sessionId, peerMac) on the RX fast path.
// ============================================================================

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stddef.h>
#include <stdint.h>

enum SessionStateLifecycle : uint8_t {
  SESSION_FREE        = 0,  // empty slot
  SESSION_ESTABLISHING= 1,  // OPEN sent / received, awaiting CONFIRM
  SESSION_ACTIVE      = 2,  // both sides have keys, ready to wrap traffic
  SESSION_REKEYING    = 3,  // Phase 3.6 — fresh keys staged, old still accepted briefly
  SESSION_CLOSED      = 4,  // explicit teardown; kept briefly to suppress late frames
};

struct SessionState {
  uint8_t  peerMac[6];
  uint8_t  meshId;
  uint8_t  myDirection;        // 0 = our MAC is the "A" side (numerically lower), 1 = "B"
  uint16_t sessionId;
  uint16_t _pad;
  uint8_t  aeadKeyTx[32];      // key used when we encrypt to the peer
  uint8_t  aeadKeyRx[32];      // key used when we decrypt from the peer
  uint32_t txSeqNext;          // next frameSeq value to use on send
  uint32_t rxSeqHighWater;     // highest frameSeq we've accepted from peer
  uint64_t rxSeqBitmap;        // 64-frame sliding window below highWater
  uint32_t establishedAtMs;    // millis() when the session went ACTIVE
  uint32_t lastUseMs;          // millis() of last TX or RX through this session
  uint8_t  state;              // SessionStateLifecycle enum
  uint8_t  _pad2[3];
};
static_assert(sizeof(SessionState) <= 128, "keep session record small");

// One-time allocation of the gSessions table in PSRAM. Idempotent; safe to
// call multiple times. Returns false only on alloc failure.
bool sessionsInit();

// Lookup by peer/mesh — used by initiator and by the send-path session-wrap
// logic in 3.5. Returns nullptr if no session exists for the pair.
SessionState* sessionFindByPeer(const uint8_t peerMac[6], uint8_t meshId);

// Lookup by sessionId (16-bit, on-wire). Fast path on RX. Returns nullptr if
// no match. peerMac filter ensures we don't confuse two peers that randomly
// landed on the same sessionId (extremely unlikely with random selection,
// but checked anyway).
SessionState* sessionFindBySessionId(uint16_t sessionId, const uint8_t peerMac[6]);

// Allocate / reuse a slot for (peerMac, meshId). Returns nullptr if cache
// full and no slot is freeable. Existing session for the pair is replaced
// (cleared). State is set to SESSION_ESTABLISHING.
SessionState* sessionAllocate(const uint8_t peerMac[6], uint8_t meshId);

// Tear down a session and zero its key material. Idempotent.
void sessionClear(SessionState* s);

// Returns true if myMac is the "A" side (lexicographically lower MAC).
// Used both for direction-tagged KDF context selection and for the
// concurrent-handshake tiebreak (A-side wins).
bool sessionIsASide(const uint8_t myMac[6], const uint8_t peerMac[6]);

// Derive both per-direction AEAD keys from a 32-byte X25519 shared secret.
// Context strings: "esp-AtoB" (key flowing from A to B) and "esp-BtoA".
// The caller assigns aeadKeyTx / aeadKeyRx based on its own A-vs-B role.
// Returns false on KDF failure.
bool sessionDeriveAeadKeys(SessionState* s, const uint8_t shared[32]);

// Read-only iteration (for the espnowsessions CLI). i ranges 0..slotCount-1.
const SessionState* sessionAt(uint8_t i);
uint8_t sessionSlotCount();

// ---- Phase 3.5a — frame wrap / unwrap + replay window ----------------------

// Wrap a plaintext payload into AEAD ciphertext + 16-byte detached tag using
// the session's TX key. Stamps the header's sessionId, frameSeq, and the
// SESSION_FRAME flag. CRC16 is zeroed (the AEAD tag replaces it as the
// integrity check). Caller must:
//   - place the V4 header into headerInOut FIRST (this function only writes
//     the fields above).
//   - provide an output buffer of size `plaintextLen + 16` for the tag-after-
//     ciphertext layout we use on the wire.
// `outCipherWithTag` may alias `plaintext` (in-place encryption is allowed
// by ChaCha20-Poly1305 — libsodium's implementation handles it).
//
// Returns false if the session isn't ACTIVE, the AEAD primitive fails, or
// txSeqNext would wrap. On success: increments txSeqNext and updates
// lastUseMs. headerInOut->crc16 is left at zero — callers must NOT compute
// CRC for SESSION_FRAME frames.
bool sessionWrapFrame(SessionState* s,
                      struct EspNowV4Header* headerInOut,
                      const uint8_t* plaintext, uint16_t plaintextLen,
                      uint8_t* outCipherWithTag);

// Unwrap a received SESSION_FRAME. Verifies the AEAD tag (last 16 bytes of
// the payload region), then runs the replay check against the session's
// sliding 64-frame window. If both pass, writes plaintext into `outPlain`
// and advances the window.
//
//   header:        the V4 header on the wire (read-only; AAD source)
//   cipherWithTag: pointer to the payload region (cipher || tag)
//   payloadLen:    total payload length including the 16-byte tag
//   outPlain:      output buffer of at least (payloadLen - 16) bytes
//   outPlainLen:   set to (payloadLen - 16) on success
//
// Returns false on AEAD tag fail OR replay-window reject (both logged). Does
// NOT advance the window on failure.
bool sessionUnwrapFrame(SessionState* s,
                        const struct EspNowV4Header* header,
                        const uint8_t* cipherWithTag, uint16_t payloadLen,
                        uint8_t* outPlain, uint16_t* outPlainLen);

// ---- Phase 3.5 step 3 — pending-frame queue ---------------------------------
//
// When the application sends to a peer that has no ACTIVE session, the send
// path kicks a SESSION_OPEN and parks the frame here. On the SESSION_CONFIRM
// initiator-side "established" event the queue is drained for that peer.
// Slots cap is small (kPendingFrameSlots) — one slot per peer is the typical
// usage; a second queued frame for the same peer evicts the older one with a
// WARN log. Frames sit at most kPendingFrameTimeoutMs before being expired by
// the periodic sweep.

constexpr uint16_t kPendingFramePlaintextMax = 202;   // ESPNOW_V4_MAX_PAYLOAD - 16
constexpr uint32_t kPendingFrameTimeoutMs    = 5000;  // SESSION_OPEN budget

// Queue a frame for later send. Returns true on enqueue success, false if the
// queue is full and the eviction policy could not free a slot.
// `flags` should be the application's base flags WITHOUT SESSION_FRAME — the
// drain path runs the frame through sessionWrapFrame which sets it.
bool pendingFrameQueue(const uint8_t peerMac[6], uint8_t type, uint16_t flags,
                       uint32_t msgId, uint8_t ttl,
                       const uint8_t* plaintext, uint16_t plaintextLen);

// Drain all pending frames for a peer. Called from the SESSION_CONFIRM
// initiator-side "established" path. Each drained frame is wrap-and-sent via
// the v4_send_session_wrapped path; on success the slot is freed. On failure
// the slot is freed with an [ERROR] log (we don't re-queue — caller can retry
// from the application layer).
void pendingFrameDrainForPeer(const uint8_t peerMac[6]);

// Expire slots older than kPendingFrameTimeoutMs. Called from the periodic
// espnow heartbeat tick. Returns count expired (for diagnostics).
uint8_t pendingFrameTimeoutSweep(uint32_t nowMs);

// Inspector for the espnowsessions CLI / debugging.
uint8_t pendingFrameCount();

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_SESSIONS_H
