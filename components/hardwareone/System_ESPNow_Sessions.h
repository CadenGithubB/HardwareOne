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
// Sessions are enforced on the wire: sessionWrapFrame seals outbound frames as
// SESSION_FRAMEs and sessionUnwrapFrame verifies + replay-checks inbound ones
// (both declared below).
//
// Capacity: 16 slots (same as the existing peer table). A SessionState is
// 240 B — the static_assert below pins that as the ceiling — so the table costs
// 3840 B (3.75 KiB) of PSRAM. Looked up by (peerMac, meshId) on the initiator
// path, by (sessionId, peerMac) on the RX fast path.
// ============================================================================

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stddef.h>
#include <stdint.h>

#include "System_ESPNow_PendingQueue.h"  // the pending-frame queue rules
#include "System_ESPNow_PeerRestartCore.h"  // when a peer restarted, the session with it ends

enum SessionStateLifecycle : uint8_t {
  SESSION_FREE        = 0,  // empty slot
  SESSION_ESTABLISHING= 1,  // OPEN sent / received, awaiting CONFIRM
  SESSION_ACTIVE      = 2,  // both sides have keys, ready to wrap traffic
  SESSION_REKEYING    = 3,  // Phase 3.6 — fresh keys staged, old still accepted briefly
  SESSION_CLOSED      = 4,  // explicit teardown; kept briefly to suppress late frames
};

// Phase 3.6 — prev-keys retention window after a successful REKEY. Frames sent
// under the old keys before the peer learned about the rotation still decrypt
// during this window. 5 s is generous given the ~10 ms typical ESP-NOW one-way
// latency; longer windows reduce the security gain of forward secrecy.
constexpr uint32_t kRekeyPrevKeysWindowMs = 5000;
// Periodic-rekey thresholds. Either firing triggers a REKEY.
constexpr uint32_t kRekeyTxFramesThreshold = 10000;     // frames sent
constexpr uint32_t kRekeyAgeThresholdMs    = 3600000;   // 1 hour
// Don't re-initiate a rekey if one was started recently — gives the
// in-flight exchange time to complete before we re-fire on the same trigger.
constexpr uint32_t kRekeyMinIntervalMs = 30000;

// How many of OUR OWN unicast sends to a peer must time out in a row (with no
// delivered ACK in between to reset the run) before we treat the ACTIVE session
// as desynced and drop it for a re-handshake. >1 so a single bad ~10s window
// (transient RF loss / a brief peer outage) can't tear down a healthy session;
// the count resets the moment any send gets through. Tune 1..3 to taste.
constexpr uint8_t kSessionSendTimeoutsBeforeReestablish = 2;

struct SessionState {
  uint8_t  peerMac[6];
  uint8_t  meshId;
  uint8_t  myDirection;        // 0 = our MAC is the "A" side (numerically lower), 1 = "B"
  uint16_t sessionId;
  uint16_t meshKeyGen;         // meshKeysGeneration(meshId) when the keys were made (0 = none yet):
                               //   a passphrase change gives a new one, and the session is closed
  uint8_t  aeadKeyTx[32];      // key used when we encrypt to the peer
  uint8_t  aeadKeyRx[32];      // key used when we decrypt from the peer
  uint32_t txSeqNext;          // next frameSeq value to use on send
  uint32_t rxSeqHighWater;     // highest frameSeq we've accepted from peer
  uint32_t rxSeqHighWaterPrev; // replay window of aeadKeyRxPrev: high-water mark
  uint64_t rxSeqBitmap;        // 64-frame sliding window below highWater
  uint64_t rxSeqBitmapPrev;    // replay window of aeadKeyRxPrev: bitmap
  uint32_t establishedAtMs;    // millis() when the session went ACTIVE
  uint32_t lastUseMs;          // millis() of last TX or RX through this session
  uint8_t  state;              // SessionStateLifecycle enum
  uint8_t  consecutiveSendTimeouts; // our own send no-ACK run (TX-driven self-heal); reset on any delivered ACK
  uint8_t  userStarted;        // a user command started this SESSION_OPEN: its timeout is always reported
  uint8_t  openResent;         // ESTABLISHING (our OPEN): it was sent once more after a glare (at most once)

  // ---- Phase 3.6 — REKEY state -----------------------------------------------
  // Previous AEAD keys, kept valid briefly after a key swap so in-flight frames
  // sent under the old keys still decrypt. Cleared after kRekeyPrevKeysWindowMs.
  // The previous key keeps the replay window it was live with
  // (rxSeqHighWaterPrev / rxSeqBitmapPrev above): a frame that opens under it
  // is checked against, and only advances, that window, so an old-epoch frame
  // can neither replay nor seed the fresh window of the current key.
  uint8_t  aeadKeyRxPrev[32];      // previous RX key
  uint32_t prevKeysValidUntilMs;   // 0 = no prev keys; otherwise millis() deadline
  // In-flight rekey state. When this side has SENT a REKEY but not yet received
  // the peer's REKEY reply, the ephemeral private key is parked here so we can
  // complete the ECDH when the reply arrives. Zeroed once REKEY completes.
  uint8_t  rekeyEphPrivKey[32];    // X25519 priv key for our outstanding REKEY
  uint32_t rekeyInitiatedAtMs;     // 0 = no rekey outstanding; otherwise start time
  uint32_t rekeyTxSeqAtInit;       // txSeqNext snapshot at moment of REKEY signing

  // ---- Bond auth token (derived at handshake, independent of bond-mode timing)
  // KDF subkey id 3 ("esp-bond") off the same X25519 shared secret as the AEAD
  // keys. Derived for EVERY session in sessionDeriveAeadKeys so it exists no
  // matter when bond mode is enabled relative to session establishment (fixes
  // the "bonded but no session token" trap where bondconnect happens on top of
  // an already-ACTIVE mesh session and the old handshake-time derive was skipped).
  // Only consulted for the configured bond peer; harmless on other sessions.
  uint8_t  bondToken[16];
  uint8_t  bondTokenValid;         // 1 once derived
  uint8_t  _pad3[3];

  // ---- Handshake freshness ----------------------------------------------------
  // A REKEY or SESSION_OPEN is a plaintext signed frame that stays valid as long
  // as the identity keys do, so a re-delivered copy must be recognised by state:
  // see sessionClassifyPeerRekey and sessionOpenIdStale below.
  uint8_t  lastPeerRekeyNonce[16];  // nonceRekey of the last peer REKEY acted on (all zero = none)
  uint32_t rekeyReplyGraceUntilMs;  // 0 = none; while pending, rekeyEphPrivKey is the key of the
                                    //   REKEY reply we sent, and a peer REKEY arriving now answers
                                    //   that reply: it is applied and not answered again
  uint16_t recentSessionIds[4];     // ids this slot held before sessionId, newest first (0 = empty)

  // ---- The peer's capability word (System_ESPNow_SessionCapsCore.h) -------------
  // From its SESSION_OPEN (we answered) or SESSION_CONFIRM (we opened), stored
  // before the slot goes ACTIVE. A REKEY keeps it; a new handshake replaces it.
  uint32_t peerSessionCaps;
  uint8_t  peerSessionCapsConfirmed; // 1: from a CONFIRM, or a sealed frame of the session has
                                     //   opened since; 0: only a (possibly replayed) OPEN says so
};
static_assert(sizeof(SessionState) <= 248, "keep session record reasonably small");

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
// full and no slot is freeable. State is set to SESSION_ESTABLISHING.
//
// The two paths do NOT clear the same amount, which matters:
//   - Free slot: full memset — every field starts at zero.
//   - Existing session for the pair: a PARTIAL reset. It zeroes the live AEAD
//     keys, txSeqNext, the rx replay window (rxSeqHighWater + rxSeqBitmap),
//     establishedAtMs, openResent and the in-flight rekey state (rekeyEphPrivKey,
//     rekeyInitiatedAtMs, rekeyTxSeqAtInit, rekeyReplyGraceUntilMs,
//     lastPeerRekeyNonce), and pushes the sessionId it replaces onto
//     recentSessionIds (the caller then writes the new sessionId). It leaves the
//     previous RX key (aeadKeyRxPrev with its window
//     rxSeqHighWaterPrev/rxSeqBitmapPrev, prevKeysValidUntilMs), the bond token
//     (bondToken, bondTokenValid) and consecutiveSendTimeouts exactly as they
//     were.
//
// The retained prev-RX key is the sharp edge: sessionUnwrapFrame still falls
// back to aeadKeyRxPrev while prevKeysValidUntilMs hasn't expired, so a
// re-allocated session can decrypt frames keyed to the session it replaced for
// up to kRekeyPrevKeysWindowMs past the last REKEY. Don't assume a re-allocated
// slot is a blank one — sessionClear is the call that fully zeroes.
SessionState* sessionAllocate(const uint8_t peerMac[6], uint8_t meshId);

// Tear down a session and zero its key material. Idempotent.
void sessionClear(SessionState* s);

// Returns true if myMac is the "A" side (lexicographically lower MAC).
// Used both for direction-tagged KDF context selection and for the
// concurrent-handshake tiebreak (A-side wins).
bool sessionIsASide(const uint8_t myMac[6], const uint8_t peerMac[6]);

// The session key derivation (1.0, C2; System_ESPNow_HandshakeCore.h): prk =
// HMAC-SHA256(bootstrapKey, "hw1-espnow-1-session" || shared), then the Blake2b
// subkeys "esp-AtoB" (id 1), "esp-BtoA" (id 2) and, when bondToken is given,
// "esp-bond" (id 3, first 16 bytes). The mesh bootstrap key is mixed in, so a
// device without the current passphrase cannot derive the keys even if it
// could finish the exchange. False on any failure (outputs zeroed).
bool sessionDeriveKeyMaterial(const uint8_t bootstrapKey[32], const uint8_t shared[32],
                              uint8_t keyAtoB[32], uint8_t keyBtoA[32], uint8_t bondToken[16]);

// Derive both per-direction AEAD keys (and the bond token) from a 32-byte X25519
// shared secret under the bootstrap key of the session's mesh slot (s->meshId),
// and record that slot's key generation in s->meshKeyGen. The caller's
// A-vs-B role (s->myDirection) picks Tx / Rx. False when the mesh has no keys
// (no passphrase: no session opens on it) or the derivation fails.
bool sessionDeriveAeadKeys(SessionState* s, const uint8_t shared[32]);

// Close every session on mesh slot `meshSlot` (a passphrase or label change:
// those sessions were keyed under the old mesh key). Returns how many closed.
// Peers that have the new passphrase open new sessions by themselves.
uint8_t sessionCloseMesh(uint8_t meshSlot);

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

// The ACK-only exception (1.0, DESIGN 2.2): an ACK frame is also sealed while
// the session is REKEYING, under the current TX key, so a frame that arrives
// during a rekey is still answered (the receive side opens sealed frames in
// ACTIVE and REKEYING). The state is checked and frameSeq taken under the
// session TX lock. Any other frame keeps sessionWrapFrame's ACTIVE-only rule.
bool sessionWrapAck(SessionState* s,
                    struct EspNowV4Header* headerInOut,
                    const uint8_t* plaintext, uint16_t plaintextLen,
                    uint8_t* outCipherWithTag);

// Unwrap a received SESSION_FRAME. Verifies the AEAD tag (last 16 bytes of
// the payload region), then runs the replay check against the sliding
// 64-frame window of the key that opened the frame: the current key's window,
// or, inside kRekeyPrevKeysWindowMs after a REKEY, the previous key's own
// window. If both pass, writes plaintext into `outPlain` and advances that
// window only.
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
// path kicks a SESSION_OPEN (or KEY_EX) and parks the frame here. On the
// SESSION_CONFIRM initiator-side "established" event the queue is drained for
// that peer, oldest frame first. The rules (System_ESPNow_PendingQueue.h): up
// to kPendingFramesPerPeer frames per peer, kPendingFrameSlots in all; a frame
// that does not fit is refused (the newest, never one already queued), marked
// failed and counted in a once-a-minute warning. Frames sit at most
// kPendingFrameTimeoutMs before the periodic sweep drops them, each marked
// failed.

constexpr uint16_t kPendingFramePlaintextMax = 202;   // ESPNOW_V4_MAX_PAYLOAD - 16
constexpr uint32_t kPendingFrameTimeoutMs    = 5000;  // SESSION_OPEN budget

// Queue a frame for later send. Returns true when it was queued. False when it
// was refused: bad arguments, the table could not be allocated (*outAdmit
// left as the caller set it), or the peer's queue or the table is full
// (*outAdmit says which; the frame's tracked send, if any, is marked failed
// and a counted warning goes out once a minute).
// `flags` should be the application's base flags WITHOUT SESSION_FRAME — the
// drain path runs the frame through sessionWrapFrame which sets it.
bool pendingFrameQueue(const uint8_t peerMac[6], uint8_t type, uint16_t flags,
                       uint32_t msgId, uint8_t ttl,
                       const uint8_t* plaintext, uint16_t plaintextLen,
                       PendingAdmit* outAdmit = nullptr);

// Drain all pending frames for a peer, oldest first. Called from the
// SESSION_CONFIRM initiator-side "established" path, the REKEY apply, the
// KEY_EX_REPLY of a peer whose session is already up, and by a sender that
// finds the session ACTIVE while frames still wait (so a new frame never
// overtakes them), and the periodic tick for a settled session
// (pendingFrameDrainSettled). Each drained frame is wrap-and-sent via the
// v4_send_session_wrapped path; on success the slot is freed. On failure the
// slot is freed with an [ERROR] log and the frame marked failed (we don't
// re-queue — caller can retry from the application layer).
void pendingFrameDrainForPeer(const uint8_t peerMac[6]);

// Expire slots older than kPendingFrameTimeoutMs, each marked failed. Called
// from the periodic espnow heartbeat tick. Returns count expired.
uint8_t pendingFrameTimeoutSweep(uint32_t nowMs);

// Send the frames that still wait for a peer whose session is ACTIVE and at
// least kPendingSessionSettleMs old (pendingSessionSettled), oldest first: a
// session this device answered (the responder of the peer's SESSION_OPEN, or
// the B side of a glare) drains nothing when it opens, and a frame queued just
// after a drain would otherwise wait for its timeout. Called from the periodic
// espnow heartbeat tick, before the timeout sweep. Returns how many went to
// the drain.
uint8_t pendingFrameDrainSettled(uint32_t nowMs);

// Reset sessions stuck in ESTABLISHING past a timeout (our SESSION_OPEN got no
// CONFIRM — peer was offline/booting when we kicked it). Without this the slot
// stays ESTABLISHING forever and v4_send_encrypted_or_queue only re-kicks from
// FREE/CLOSED, so the bond can never re-establish. Called from the heartbeat
// tick. Returns count reset.
uint8_t sessionEstablishingTimeoutSweep(uint32_t nowMs);

// A user command (espnowsessionopen, bondconnect, bondresync) started or
// joined the SESSION_OPEN to `peerMac` that is now ESTABLISHING: if it times
// out, the warning is printed at once instead of going through the 15-minute
// limit of automatic tries. cmd_exec.
void sessionMarkUserStarted(const uint8_t peerMac[6]);

// True while a handshake on this slot is under way and must be left alone: a
// SESSION_OPEN we sent that is still inside its CONFIRM budget (ESTABLISHING),
// or a REKEY awaiting the peer's answer (REKEYING, which still holds the
// working keys). espnowSessionOpenInitiate starts nothing while this holds,
// because sessionAllocate on the slot would zero the keys that exchange needs.
// Ending a handshake that never completes stays with the two sweeps
// (sessionEstablishingTimeoutSweep, rekeyRetrySweep). A slot they did not get
// to - ESTABLISHING past the CONFIRM budget, REKEYING past twice the REKEY
// retry budget - is stale and reads false, so the next kick replaces it instead
// of waiting for ever. nullptr, FREE, ACTIVE and CLOSED read false.
bool sessionHandshakeInFlight(const SessionState* s, uint32_t nowMs);

// Inspector for the espnowsessions CLI / debugging.
uint8_t pendingFrameCount();

// Frames waiting for `peerMac`'s session.
uint8_t pendingFrameCountForPeer(const uint8_t peerMac[6]);

// True iff at least one pending frame is parked waiting for `peerMac`'s session.
// Called from KEY_EX_REPLY's handler (initiator side, post-peerIdentityPersist)
// to decide whether to auto-kick SESSION_OPEN — the "encrypt-or-wait" foundation.
bool pendingFrameHasForPeer(const uint8_t peerMac[6]);

// True while the frame `msgId` to `peerMac` waits in the queue. A command that
// handed a frame to the send path asks this to answer "queued until the
// session opens" instead of "sent". A frame drained in between reads false,
// which is then true: it went out.
bool pendingFrameIsWaiting(uint32_t msgId, const uint8_t peerMac[6]);

// Unpair: drops the frames waiting for `peerMac`'s session (each is marked
// failed in the tracked-send table), so a session that comes up later does
// not deliver them to a device the user unpaired. Returns how many went.
uint8_t pendingFrameDropForPeer(const uint8_t peerMac[6]);

// closeespnow: drops every waiting frame (each marked failed), so a frame
// queued before the close never goes out after a later openespnow. Returns
// how many went.
uint8_t pendingFrameDropAll();

// ---- Phase 3.5 task #49 — tracked-send status (web-UI delivery confirmation)
//
// When the CLI accepts a user-initiated send, it registers the msgId+peerMac
// here. The V4_ACK_RX path marks the entry DELIVERED on a matching ACK. The
// pending-frame timeout-sweep or an early send failure marks it FAILED. A
// periodic sweep ages out PENDING entries that never got an ACK. The web UI
// reads the whole table out of the ESP-NOW messages JSON snapshot (the
// "deliveries" array built in handleEspNowMessages) and matches on msgId to
// drive the chat bubble's ✓ → ✓✓. There is no per-msgId HTTP endpoint.
//
// Capacity is small (kSendStatusSlots) and the table is a ring — newest
// registrations evict the oldest entry. Web UI polling caps at ~15 attempts
// (7.5s) so client gives up before the 30s server sweep would otherwise
// expire a still-being-watched entry.

constexpr uint32_t kSendStatusPendingTimeoutMs = 10000;  // PENDING → TIMEOUT
constexpr uint32_t kSendStatusKeepResolvedMs   = 30000;  // hold resolved entries this long for the poll window

enum SendStatusState : uint8_t {
  SEND_STATUS_PENDING   = 0,
  SEND_STATUS_DELIVERED = 1,
  SEND_STATUS_TIMEOUT   = 2,
  SEND_STATUS_FAILED    = 3,
};

struct SendStatus {
  bool     inUse;
  uint8_t  state;          // SendStatusState
  uint8_t  peerMac[6];
  uint32_t msgId;
  uint32_t registeredAtMs;
  uint32_t resolvedAtMs;   // 0 while PENDING
};

// Register a new tracked send. Called from cmd_espnow_send after the send
// (or queueing) has been accepted. Always succeeds (oldest entry evicted on
// overflow).
void sendStatusRegister(uint32_t msgId, const uint8_t peerMac[6]);

// Mark the matching tracked send as DELIVERED. Called from V4_ACK_RX.
// Match key is (msgId AND peerMac). No-op if no matching PENDING entry.
void sendStatusMarkDelivered(uint32_t msgId, const uint8_t srcMac[6]);

// Mark the matching tracked send FAILED because the peer answered it with a
// sealed ACK carrying a status (a refusal, 1.0). Unlike sendStatusMarkFailed
// it also counts the answer as proof that the session is alive (it clears the
// no-answer run, as a delivery does): only silence counts toward a re-open.
void sendStatusMarkRefused(uint32_t msgId, const uint8_t srcMac[6]);

// Mark a tracked send as FAILED. Called from the pending-frame timeout sweep
// when a queued encrypted frame gets dropped (handshake never completed).
void sendStatusMarkFailed(uint32_t msgId, const uint8_t peerMac[6]);

// Periodic sweep — PENDING → TIMEOUT after kSendStatusPendingTimeoutMs,
// RESOLVED entries cleared after kSendStatusKeepResolvedMs. Called from the
// espnow heartbeat tick.
void sendStatusSweep(uint32_t nowMs);

// Single-entry lookup by msgId. Fills *out and returns true if the msgId is
// known; false if the entry has been evicted or never existed.
//
// Has NO callers — the web UI takes the snapshot-walk path below instead. Kept
// as the obvious primitive for a future by-msgId consumer, but nothing
// exercises it today, so don't treat it as a tested path.
bool sendStatusGet(uint32_t msgId, SendStatus* out);

// Walk all slots for the JSON snapshot in handleEspNowMessages. Callers do
// i = 0..sendStatusSlotCount()-1 and check returned slot's inUse flag.
extern "C" uint8_t sendStatusSlotCount();
extern "C" const SendStatus* sendStatusAt(uint8_t i);

// ---- Phase 3.6 — REKEY helpers ---------------------------------------------

// Apply a freshly-derived AEAD key pair to a session, stashing the existing
// RX key into the prev slot with a kRekeyPrevKeysWindowMs validity window.
// (TX key isn't kept — we control our own TX so old TX keys won't see use.)
// Resets txSeqNext to 0 and starts a fresh rx replay window — the new keys
// define a fresh frame-seq namespace. The window that was live with the old RX
// key moves to rxSeqHighWaterPrev / rxSeqBitmapPrev together with that key, so
// late old-key frames stay replay-checked. State transitions to SESSION_ACTIVE.
// rekeyEphPrivKey + rekeyInitiatedAtMs + rekeyTxSeqAtInit are all zeroed and
// any reply grace (rekeyReplyGraceUntilMs) ends.
void sessionApplyRekeyedKeys(SessionState* s,
                             const uint8_t newKeyTx[32],
                             const uint8_t newKeyRx[32]);

// Mark a session as having a REKEY in flight. Parks our ephemeral X25519
// priv key for the eventual ECDH when the peer's REKEY reply arrives (ending
// any reply grace). Returns false if the session isn't ACTIVE (can't rekey
// from any other state).
bool sessionMarkRekeyInitiated(SessionState* s,
                               const uint8_t ephPrivKey[32],
                               uint32_t txSeqAtSign);

// Abort an in-flight rekey: REKEYING → ACTIVE without installing new keys
// (discards the parked ephemeral priv, keeps the current AEAD keys). The
// failure leg of the rekey lifecycle; runDeferredRekey calls it on ECDH/KDF
// failure so a failed rekey can't strand the session in REKEYING. No-op if
// the session isn't currently REKEYING.
void sessionAbortRekey(SessionState* s);

// Sweep prev-keys retention windows. Called from the espnow heartbeat tick.
// Zeros expired aeadKeyRxPrev material and its replay window, and ends an
// expired reply grace (wiping the reply's parked ephemeral key).
void sessionRekeyPrevKeysSweep(uint32_t nowMs);

// ---- Handshake freshness: re-delivered REKEY / SESSION_OPEN ------------------
//
// The (origin, msgId) dedup ring only remembers a frame for 5 s, and the signed
// REKEY / OPEN transcripts carry nothing the receiver can check against time,
// so these helpers bind a received handshake frame to the session's live state.

// A fresh REKEY's prevTxSeqAtRekey (the peer's txSeqNext when it signed) may
// trail our rx high-water mark by at most this many frames.
constexpr uint32_t kRekeySeqSlack = 64;

// How a REKEY received from the peer relates to this session's live state.
enum SessionPeerRekeyVerdict : uint8_t {
  SESSION_REKEY_COUNTERPART = 0,  // answers a REKEY we sent: derive with the parked key, send nothing
  SESSION_REKEY_INITIATION  = 1,  // a fresh initiation: reply with our own REKEY, then derive
  SESSION_REKEY_NONCE_SEEN  = 2,  // same nonce as the last peer REKEY acted on (re-delivery)
  SESSION_REKEY_TOO_SOON    = 3,  // an initiation less than kRekeyMinIntervalMs after the keys changed
  SESSION_REKEY_STALE_SEQ   = 4,  // an initiation signed more than kRekeySeqSlack frames behind
};

// Classify a signature-verified REKEY from the peer. In order: the nonce of the
// last REKEY acted on → NONCE_SEEN; we are REKEYING, or inside the reply grace
// → COUNTERPART; keyed (established or rekeyed) less than kRekeyMinIntervalMs
// ago → TOO_SOON; prevTxSeqAtRekey more than kRekeySeqSlack below
// rxSeqHighWater → STALE_SEQ; otherwise INITIATION. Only COUNTERPART and
// INITIATION may change the keys.
SessionPeerRekeyVerdict sessionClassifyPeerRekey(const SessionState* s,
                                                 const uint8_t nonceRekey[16],
                                                 uint32_t prevTxSeqAtRekey,
                                                 uint32_t nowMs);

// Remember the nonce of a peer REKEY whose keys are being applied.
void sessionNotePeerRekey(SessionState* s, const uint8_t nonceRekey[16]);

// Responder side, right after sessionApplyRekeyedKeys installed the keys from
// our REKEY reply: parks the reply's ephemeral private key again and opens the
// reply grace for kRekeyPrevKeysWindowMs. A peer REKEY arriving inside it is
// the peer's answer to our reply (it treated the reply as an initiation), so it
// is applied with this key and not answered again, which ends the exchange.
void sessionHoldRekeyReply(SessionState* s, const uint8_t ephPrivKey[32]);

// True when `sessionId` is 0, the slot's current sessionId, or one of the ids
// the slot held before it. A genuine SESSION_OPEN always proposes a fresh
// random id, so such an OPEN is a re-delivered copy.
bool sessionOpenIdStale(const SessionState* s, uint16_t sessionId);

// ---- A peer restarted: the session with it ends (System_ESPNow_PeerRestartCore.h)
//
// The rules are in the core header; these own the table (PSRAM, under
// EspNowTxGuard), the session close and the lines. All run on espnow_task
// (RX dispatch), never in the radio callback.

// A group-signed BOOT notice from paired `peerMac` (the caller checked the tag
// is of the peer's mesh; a sealed notice goes to sessionPeerBootSeen). `hasCounter` false when the notice carries no boot counter. On
// a restart it closes the ACTIVE/REKEYING session this device holds with the
// peer (sends after it queue and open a new one; frames that already wait
// stay queued and go out on the new session) and prints one line naming the
// peer. Returns the verdict.
PeerBootVerdict sessionPeerBootNotice(const uint8_t peerMac[6], bool hasCounter, uint32_t bootCounter);

// A boot counter seen in a sealed frame of the live session (bond heartbeat, a
// BOOT notice sealed in the session): becomes the reference outright (lower
// ones included: the peer's count was erased); never ends a session.
void sessionPeerBootSeen(const uint8_t peerMac[6], uint32_t bootCounter);

// This device holds an ACTIVE or REKEYING session with paired `peerMac` (the
// slot of its identity's mesh). meshSendBootToPeers seals the notice then.
bool sessionLiveWithPeer(const uint8_t peerMac[6]);

// The receive path found no ACTIVE/REKEYING session for a sealed frame from
// `srcMac`: answers with a SESSION_UNKNOWN hint when the rules say so (the
// peer has a stored identity, the id is not this device's handshake in flight
// or a replaced session, at most one per peer per second). Returns true when a
// hint went out.
bool sessionUnknownFrameSeen(const uint8_t srcMac[6], const struct EspNowV4Header* h);

// A SESSION_UNKNOWN hint from paired `srcMac` (group tag checked by the row;
// v4h_session_unknown checked it is of the peer's mesh and from the peer itself).
// Closes the named live session, marks hint->msgId failed, prints one line and
// starts a SESSION_OPEN when the rules say so. Returns the verdict.
SessionHintVerdict sessionUnknownHintReceived(const uint8_t srcMac[6],
                                              const struct V4PayloadSessionUnknown* hint);

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_SESSIONS_H
