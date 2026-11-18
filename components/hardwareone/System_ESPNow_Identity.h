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
// This module generates, loads, and exposes the keypair; the on-wire crypto
// that consumes it lives elsewhere. The public key rides KEY_EX_HELLO/REPLY so
// peers can learn and persist it. The secret signs the SESSION_OPEN,
// SESSION_CONFIRM and SESSION_REKEY transcripts — that signature is what lets a
// peer trust that an ephemeral X25519 key really came from this device.
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
// This function itself only touches OUR identity — it does not walk the peer
// table. The CLI wrapper is what honours `--confirm-wipe-all-bonds`: after a
// successful regenerate it calls peerIdentityForget() for every valid slot,
// deleting each `/system/espnow/peers/<mac>/identity.json`.
//
// That wipe is not housekeeping. A new identity means every peer still holds
// our OLD pubkey, so the trust is already dead in both directions; the stale
// records would sit `valid` forever, and findFreeSlot() hands out the first
// !valid slot — a full table of dead entries would lock out every future
// pairing. If you ever call this outside the CLI, do the same, or accept that
// the peer table silently fills with bonds no peer can honour.
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

// Phase 5 — event subscription registry. subscribedEvents is a bitmask of
// EspNowEventCategory bits (see below). When THIS device wants to broadcast
// e.g. HEARTBEAT, it iterates paired peers and skips any whose bitmask omits
// the HEARTBEAT bit. The peer told us their preferences via SUBSCRIBE_UPDATE
// (ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE = 130).
//
// Default for newly-paired peers OR peers loaded from a version-1 identity.json
// (pre-Phase-5) is 0xFFFFFFFF — subscribe to everything. Phase 5 only matters
// once a peer sends an explicit SUBSCRIBE_UPDATE narrowing the set.
struct PeerIdentity {
  uint8_t  mac[6];
  uint8_t  meshId;            // mesh slot index (0..N_MESHES-1)
  uint8_t  _pad;
  uint8_t  longTermPub[32];   // peer's Ed25519 public key
  uint32_t bondedAtSec;       // Unix epoch when first paired
  uint32_t lastSeenSec;       // time of the last KEY_EX that wrote the file (hourly at most)
  uint32_t subscribedEvents;  // Phase 5 — events this peer wants from US
  bool     valid;             // false = slot is empty
};

// Phase 5 — event category bitmask values. Stored in PeerIdentity.subscribedEvents
// and carried on the wire in V4PayloadSubscribe.requestedEvents. New categories
// must be added at the END to keep the bit assignment stable across firmware
// versions (peers will exchange bitmaps blind, so reordering is breaking).
enum EspNowEventCategory : uint32_t {
  ESPNOW_EVT_HEARTBEAT       = 1u << 0,   // V4 heartbeat (type 30)
  ESPNOW_EVT_SENSOR          = 1u << 1,   // was the sensor status broadcast (151), which is now a session
                                          // unicast to the receiver of the readings — bit kept, no sender
  ESPNOW_EVT_TOPOLOGY        = 1u << 2,   // topo req/start/peer (32-34)
  ESPNOW_EVT_BOND_HEARTBEAT  = 1u << 3,   // bond heartbeat (170)
  ESPNOW_EVT_WORKER_STATUS   = 1u << 4,   // retired opcode (ex-WORKER_STATUS) — bit kept for bitmap stability
  ESPNOW_EVT_METADATA_PUSH   = 1u << 5,   // metadata push (55)
  ESPNOW_EVT_TIME_SYNC       = 1u << 6,   // was the time broadcast (35), which is now a session unicast
                                          // from the master — bit kept, no sender
  // Add new categories here; preserve existing bit values.
  ESPNOW_EVT_ALL             = 0xFFFFFFFFu  // default for legacy / pre-Phase-5 peers
};

// Lookup by MAC. Returns nullptr if no match. Lifetime is global (slot pointer
// stays valid for the life of the process, but the slot's contents may change
// if KEY_EX with the same peer runs again).
const PeerIdentity* peerIdentityFindByMac(const uint8_t mac[6]);

// Store / overwrite the peer's identity: the in-memory cache and the on-disk
// file, which always hold the same record. A call that changes nothing stored
// (same key, same mesh) writes nothing and returns true, except that it moves
// lastSeenSec forward once the stored value is an hour or more behind. Returns
// false when the cache is full or the write fails; the cache is then left as
// it was.
bool peerIdentityPersist(const uint8_t mac[6], uint8_t meshId,
                         const uint8_t pub[32], uint32_t bondedAtSec);


// Phase 5 — record the peer's subscription bitmap and persist immediately.
// Called from the SUBSCRIBE_UPDATE handler. Returns false if the peer isn't
// known (no PeerIdentity slot — drop silently from the handler) or if
// persistence fails.
bool peerIdentitySetSubscriptions(const uint8_t mac[6], uint32_t subscribedEvents);

// Phase 5 — gating helper for the broadcast paths. Returns true if `category`
// is present in the peer's subscribedEvents bitmap. Unknown peers (no
// PeerIdentity) get the all-1s default — backward compat: a peer that never
// sent SUBSCRIBE_UPDATE receives everything.
bool peerIdentityWantsEvent(const uint8_t mac[6], uint32_t category);

// Forget a paired peer. Removes the on-disk file AND clears the in-memory
// slot. Idempotent. Returns true when there was something to forget: a cached
// identity, or an identity.json that is now gone (also one the boot walk did
// not load); false for a peer with neither.
bool peerIdentityForget(const uint8_t mac[6]);

// True when an identity.json is stored for the peer. After peerIdentityForget
// it says the file would not go away: the peer loads again at the next start.
bool peerIdentityFileStored(const uint8_t mac[6]);

// What peerFilesRemove took away.
struct PeerFilesRemoved {
  uint16_t files;       // files removed from the peer's folder
  uint32_t bytes;       // their size in total
  uint8_t  failed;      // entries that would not go
  bool     folderLeft;  // the folder is still there afterwards
};

// Everything this device stored about a peer: forgets its identity
// (peerIdentityForget), then removes every other file under
// /system/espnow/peers/<MAC>/ (the bond caches settings.json and schema.json,
// the manifest reference) and the folder. Used by espnowunpair; espnowforget
// only forgets the identity. Files received from the peer (/espnow/received/)
// are the user's and are not touched. Returns false when the folder stays.
bool peerFilesRemove(const uint8_t mac[6], PeerFilesRemoved& out);

// Walk /system/espnow/peers/*/identity.json at boot. Populates the cache.
// Called once after espnowIdentityLoadOrGenerate. Returns the number of
// peer identities loaded. Only folders named exactly as macToPathToken writes
// them are read (one cache entry per address); a file whose meshId is not a
// slot of this firmware is left out like an unreadable one.
uint8_t peerIdentityLoadAll();

// True once peerIdentityLoadAll has read every stored identity this boot (an
// absent peers directory counts). False when it never ran, could not open the
// directory, or left a stored identity out (unreadable file, full cache): the
// cache then does not say which mesh slots paired devices use.
bool peerIdentitiesLoaded();

// Read-only iteration: returns the i-th valid entry or nullptr if i is past
// the end. Order is not meaningful (insertion order in the cache).
const PeerIdentity* peerIdentityAt(uint8_t i);

// Number of allocated peer identity slots. Hard-coded to 16 in the .cpp
// (kPeerSlots), matching the existing peer-table cap (gEspNow->devices[16] /
// MESH_PEER_MAX). It is NOT derived from N_MESHES — raising N_MESHES would
// silently leave this table at 16.
uint8_t peerIdentitySlotCount();

// The "version" each file kind is written with and read back as, for
// `versions`: this device's identity.json and the per-peer identity files.
uint8_t espnowIdentityFileVersion();
uint8_t peerIdentityFileVersion();

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_IDENTITY_H
