#ifndef SYSTEM_ESPNOW_IDENTITY_H
#define SYSTEM_ESPNOW_IDENTITY_H

// ============================================================================
// ESP-NOW V4 Phase 3.0 — per-device long-term cryptographic identity.
//
// Owns one Ed25519 keypair generated at first boot, persisted to
//   /system/espnow/identity.json
// with the 64-byte secret AES-128-CBC-encrypted at rest using the same
// eFuse+flash-UID-derived device key that the existing settings registry
// uses for `isSecret` fields.
//
// Phase 3.0 only generates, loads, and exposes the keypair. No on-wire
// crypto consumes it yet — that comes in 3.3 (KEY_EX handshake) and 3.4
// (SESSION_OPEN). Splitting them out keeps each commit independently
// verifiable.
//
// Boot rule: identity_load_or_generate() runs after the filesystem is
// mounted and BEFORE ESPNOW initializes. If it fails (corrupt JSON, AES
// decrypt failure), ESPNOW does not start — the device surfaces a
// recoverable error rather than silently regenerating and losing every
// existing peer's trust. The `espnowregenidentity --confirm-wipe-all-bonds`
// CLI is the manual recovery path.
// ============================================================================

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stddef.h>
#include <stdint.h>

struct EspNowIdentity {
  uint8_t  pub[32];          // crypto_sign_PUBLICKEYBYTES
  uint8_t  sec[64];          // crypto_sign_SECRETKEYBYTES (Ed25519 secret;
                             //   the trailing 32 bytes mirror `pub` per
                             //   libsodium convention — do not separate)
  uint32_t createdAtSec;     // Unix epoch when this keypair was generated
  uint32_t regenCount;       // Bumped every time the keypair is regenerated
                             // via the CLI recovery path (zero on first boot)
  bool     valid;            // false until load_or_generate succeeds
};

// Load identity from /system/espnow/identity.json. If the file is absent,
// generate a fresh keypair and persist it (regenCount = 0). If the file
// exists but is corrupt, return false WITHOUT auto-regenerating — the
// boot path is expected to refuse to start ESPNOW and surface the error
// for manual recovery.
//
// On success, also populates the module-internal global so
// espnowIdentityGet() returns the same data. Returns true on success.
bool espnowIdentityLoadOrGenerate(EspNowIdentity& out);

// Force-regenerate the identity. Bumps regenCount. Overwrites the on-disk
// file atomically. Called only by the `espnowregenidentity` CLI.
//
// Does NOT delete peer trust records (`/system/espnow/peers/<mac>/identity.json`)
// — those don't exist until Phase 3.2. The flag the CLI requires
// (`--confirm-wipe-all-bonds`) is forward-looking; once 3.2 lands the
// delete-peer-records logic will live there.
bool espnowIdentityRegenerate(EspNowIdentity& out);

// Accessor for the module-internal global. `valid` is false until
// espnowIdentityLoadOrGenerate has returned true at least once. Lifetime
// is for the whole process — callers can hold the reference.
const EspNowIdentity& espnowIdentityGet();

// Hex-encode the pubkey for display (`espnowidentity` CLI etc.). Output
// is exactly 64 lowercase hex chars + NUL; pass a buffer of at least 65.
void espnowIdentityFormatPubHex(const uint8_t pub[32], char* out, size_t outLen);

// ============================================================================
// Phase 3.2 — per-peer long-term identity record.
//
// One file per paired peer at /system/espnow/peers/<MAC>/identity.json:
//   { version, mac, meshId, longTermPubEd25519_hex, bondedAtSec, lastSeenSec }
// Plain JSON, no encryption at rest — the peer's public key is, by definition,
// public. An attacker with flash access learns which peers we've paired with;
// that's the same threat surface as the existing /system/espnow/devices.json.
//
// In-memory cache: gPeerIdentities[N] (PSRAM, parallel to gEspNow->devices[16]).
// Looked up by MAC. Loaded at boot from disk. Updated when KEY_EX completes
// successfully (3.3); cleared by espnowforget.
// ============================================================================

struct PeerIdentity {
  uint8_t  mac[6];
  uint8_t  meshId;            // mesh slot index (0..N_MESHES-1)
  uint8_t  _pad;
  uint8_t  longTermPub[32];   // peer's Ed25519 public key
  uint32_t bondedAtSec;       // Unix epoch when first paired
  uint32_t lastSeenSec;       // updated by heartbeats / app frames (3.4+)
  bool     valid;             // false = slot is empty
};

// Lookup by MAC. Returns nullptr if no match. Lifetime is global (slot pointer
// stays valid for the life of the process, but the slot's contents may change
// if KEY_EX with the same peer runs again).
const PeerIdentity* peerIdentityFindByMac(const uint8_t mac[6]);

// Store / overwrite the peer's identity. Writes the on-disk file AND updates
// the in-memory cache. Returns false on any I/O or alloc failure.
bool peerIdentityPersist(const uint8_t mac[6], uint8_t meshId,
                         const uint8_t pub[32], uint32_t bondedAtSec);

// Update lastSeenSec for an existing peer. No-op if peer not found. Does not
// persist to disk on every call — caller controls when to flush (typically
// on bond/disconnect, not per heartbeat).
void peerIdentityNoteSeen(const uint8_t mac[6], uint32_t nowSec);

// Forget a paired peer. Removes the on-disk file AND clears the in-memory
// slot. Idempotent — calling on a non-existent peer just returns true.
bool peerIdentityForget(const uint8_t mac[6]);

// Walk /system/espnow/peers/*/identity.json at boot. Populates the cache.
// Called once after espnowIdentityLoadOrGenerate. Returns the number of
// peer identities loaded.
uint8_t peerIdentityLoadAll();

// Read-only iteration: returns the i-th valid entry or nullptr if i is past
// the end. Order is not meaningful (insertion order in the cache).
const PeerIdentity* peerIdentityAt(uint8_t i);

// Number of allocated peer identity slots. Same as N_MESHES * 4 for now —
// matches the existing peer-table cap.
uint8_t peerIdentitySlotCount();

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_IDENTITY_H
