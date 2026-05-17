#ifndef SYSTEM_ESPNOW_HANDLERS_CRYPTO_H
#define SYSTEM_ESPNOW_HANDLERS_CRYPTO_H

// ============================================================================
// ESP-NOW V4 Phase 3.3 — KEY_EX handshake handlers + initiator.
//
// This module owns the receive-side KEY_EX_HELLO/REPLY/CONFIRM handlers and
// the initiator-side `espnowKeyExInitiate()` entrypoint. Both ends share
// helpers for HMAC computation/verification and peer-identity persistence.
//
// The handlers themselves are exposed for registration in the V4 dispatch
// table (see kV4HandlerTable in System_ESPNow.cpp). The struct V4RxCtx
// signature lives in System_ESPNow.cpp; the handler functions take it by
// reference and we keep the surface area here narrow.
// ============================================================================

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stdint.h>

struct V4RxCtx;  // defined in System_ESPNow.cpp

// Receive-side handlers. Each:
//   - Validates the payload struct size.
//   - Resolves mesh by header.meshFingerprint; rejects if unknown.
//   - Verifies HMAC against the mesh bootstrap key. Loud reject on fail.
//   - Persists the peer's pubkey to /system/espnow/peers/<MAC>/identity.json
//     (HELLO + REPLY only — CONFIRM is just a status acknowledgement).
//   - Sends the next step in the handshake (HELLO → REPLY → CONFIRM).
void v4hKeyExHello   (const V4RxCtx& ctx);
void v4hKeyExReply   (const V4RxCtx& ctx);
void v4hKeyExConfirm (const V4RxCtx& ctx);

// Initiator entry point. Build KEY_EX_HELLO with our pubkey + HMAC over the
// mesh bootstrap key, then send to `peerMac`. Mesh slot is identified by
// label; pass empty string to use the configured default mesh.
//
// Returns true on send success — NOT on handshake completion (the REPLY
// arrives asynchronously and runs through v4hKeyExReply). The caller can
// optionally watch for the corresponding identity file to appear under
// /system/espnow/peers/<mac>/ to confirm.
bool espnowKeyExInitiate(const uint8_t peerMac[6], const char* meshLabel);

// ---- Phase 3.4 — SESSION handshake -----------------------------------------

// Receive handlers for SESSION_OPEN / SESSION_CONFIRM. Both verify the
// Ed25519 signature against the peer's long-term pubkey (stored from KEY_EX)
// over the canonical transcript. On success, derive AEAD session keys via
// Blake2b KDF and flip the SessionState to ACTIVE.
void v4hSessionOpen   (const V4RxCtx& ctx);
void v4hSessionConfirm(const V4RxCtx& ctx);

// Initiator entrypoint. Requires the peer to already have an identity record
// from a prior successful KEY_EX (Phase 3.3). Allocates / replaces the
// SessionState slot, generates a fresh ephemeral X25519 keypair + random
// nonceA + random sessionId, signs the OPEN transcript with our long-term
// Ed25519 secret, and sends. The session goes to ACTIVE when CONFIRM lands.
bool espnowSessionOpenInitiate(const uint8_t peerMac[6], const char* meshLabel);

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_HANDLERS_CRYPTO_H
